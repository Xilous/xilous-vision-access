// xva-setup: installs the signed VMulti driver that XVA outputs through, or removes it again.
//
//   xva-setup              install VMulti, unless a working VMulti device is already present
//   xva-setup /uninstall   remove VMulti, but only if this setup installed it
//
// The package comes from VMulti.Driver.zip beside this program when present, and is otherwise
// downloaded from its original release. Either way it is copied into a folder only administrators
// can write to, checked against a pinned SHA-256 hash and Microsoft's catalog signature, and
// installed the way devcon would. Setup then confirms that Windows actually started the driver.
//
// Exit codes: 0 success, 1 failure, 2 unsupported PC, 3010 success but Windows needs a restart.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <wincrypt.h>
#include <initguid.h>
#include <devpkey.h>
#include <bcrypt.h>
#include <newdev.h>
#include <sddl.h>
#include <setupapi.h>
#include <softpub.h>
#include <urlmon.h>
#include <wintrust.h>

#include "vmulti_device.hpp"

#include <cstdio>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// X9VoiD/vmulti-bin release v1.0, as inspected and tested; its asset was last replaced on 2023-10-31.
constexpr wchar_t kPackageUrl[] = L"https://github.com/X9VoiD/vmulti-bin/releases/download/v1.0/VMulti.Driver.zip";
constexpr wchar_t kPackageFile[] = L"VMulti.Driver.zip";
constexpr wchar_t kPackageSha256[] = L"cc34f74a6bee7f3d1fdc3c10aae27118a359f56a51de2f5965b7d0d3e353d3a1";
constexpr wchar_t kInfFile[] = L"vmulti.inf";
constexpr wchar_t kCatalogFile[] = L"pentablethid.cat";
constexpr wchar_t kCatalogSigner[] = L"Microsoft Windows Hardware Compatibility Publisher";
constexpr wchar_t kHardwareId[] = L"pentablet\\hid";

// Where setup records the device it created, so /uninstall never removes one it did not install.
constexpr wchar_t kProductKey[] = L"SOFTWARE\\XilousVisionAccess";
constexpr wchar_t kOwnershipKey[] = L"SOFTWARE\\XilousVisionAccess\\Setup";

constexpr DWORD kMaxDeviceIdLength = 200;  // MAX_DEVICE_ID_LEN
constexpr DWORD kStartTimeoutMs = 15000;

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUnsupported = 2;
constexpr int kExitRebootRequired = 3010;  // ERROR_SUCCESS_REBOOT_REQUIRED, the installer convention

struct SetupError {
    int exit_code;
    std::wstring message;
};

std::wstring system_message(DWORD code) {
    wchar_t *text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<wchar_t *>(&text), 0, nullptr);
    std::wstring message;
    if (length) {
        message.assign(text, length);
        while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' ')) {
            message.pop_back();
        }
    } else {
        wchar_t hex[16];
        swprintf(hex, 16, L"0x%08X", code);
        message = L"error ";
        message += hex;
    }
    if (text) LocalFree(text);
    return message;
}

[[noreturn]] void fail_code(const wchar_t *what, DWORD code) {
    throw SetupError{kExitFailed, std::wstring(what) + L": " + system_message(code)};
}

[[noreturn]] void fail_last_error(const wchar_t *what) {
    fail_code(what, GetLastError());
}

void say(const std::wstring &text) {
    wprintf(L"%ls\n", text.c_str());
}

bool is_x64_pc() {
    USHORT process_machine = 0;
    USHORT native_machine = 0;
    if (IsWow64Process2(GetCurrentProcess(), &process_machine, &native_machine)) {
        return native_machine == IMAGE_FILE_MACHINE_AMD64;
    }
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    return info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
}

bool vmulti_working() {
    const HANDLE control = xva::open_vmulti_control(0);
    if (control == INVALID_HANDLE_VALUE) return false;
    CloseHandle(control);
    return true;
}

bool vmulti_device_exists() {
    const HDEVINFO devices = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) fail_last_error(L"Listing devices");

    bool found = false;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(devices, i, &device); ++i) {
        wchar_t ids[1024] = {};  // a list of strings ending in an empty one
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID, nullptr,
                                               reinterpret_cast<BYTE *>(ids), sizeof(ids) - 2 * sizeof(wchar_t),
                                               nullptr)) {
            continue;
        }
        for (const wchar_t *id = ids; *id && !found; id += wcslen(id) + 1) {
            found = _wcsicmp(id, kHardwareId) == 0;
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return found;
}

std::wstring program_folder() {
    std::vector<wchar_t> path(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) fail_last_error(L"Finding setup's own folder");
        if (length < path.size()) break;
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path.data()).parent_path().wstring();
}

// A fresh folder in C:\Windows\Temp that only Administrators and SYSTEM can change. Unelevated
// users cannot delete or rename it there, so nothing can swap the files between verifying them
// and installing them.
class WorkFolder {
public:
    WorkFolder() {
        wchar_t windows[MAX_PATH];
        if (!GetWindowsDirectoryW(windows, MAX_PATH)) fail_last_error(L"Finding the Windows folder");
        path_ = std::wstring(windows) + L"\\Temp\\xva-setup-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64());

        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)",
                                                                  SDDL_REVISION_1, &descriptor, nullptr)) {
            fail_last_error(L"Preparing a protected working folder");
        }
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        const BOOL created = CreateDirectoryW(path_.c_str(), &attributes);
        const DWORD error = created ? ERROR_SUCCESS : GetLastError();
        LocalFree(descriptor);
        if (!created) fail_code(L"Creating a protected working folder", error);
    }

    ~WorkFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    WorkFolder(const WorkFolder &) = delete;
    WorkFolder &operator=(const WorkFolder &) = delete;

    const std::wstring &path() const { return path_; }
    std::wstring file(const wchar_t *name) const { return path_ + L"\\" + name; }

private:
    std::wstring path_;
};

void download(const wchar_t *url, const std::wstring &destination) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT result = URLDownloadToFileW(nullptr, url, destination.c_str(), 0, nullptr);
    if (SUCCEEDED(com)) CoUninitialize();
    if (FAILED(result)) fail_code(L"Downloading the VMulti package", static_cast<DWORD>(result));
}

std::wstring sha256_hex(const std::wstring &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw SetupError{kExitFailed, L"Reading the VMulti package failed"};
    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    UCHAR digest[32];
    const NTSTATUS status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(data.data()),
                                       static_cast<ULONG>(data.size()), digest, sizeof(digest));
    if (!BCRYPT_SUCCESS(status)) fail_code(L"Hashing the VMulti package", static_cast<DWORD>(status));

    std::wstring hex;
    for (const UCHAR byte : digest) {
        wchar_t pair[3];
        swprintf(pair, 3, L"%02x", byte);
        hex += pair;
    }
    return hex;
}

void unpack(const std::wstring &zip, const std::wstring &folder) {
    // tar.exe ships with Windows 10 1803 and later and unpacks zip files. It is started by its full
    // path in System32 so no other tar.exe can stand in for it.
    wchar_t system[MAX_PATH];
    if (!GetSystemDirectoryW(system, MAX_PATH)) fail_last_error(L"Finding the System32 folder");
    const std::wstring tar = std::wstring(system) + L"\\tar.exe";
    std::wstring command = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + folder + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        fail_last_error(L"Starting tar to unpack the VMulti package");
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exit_code != 0) {
        throw SetupError{kExitFailed, L"Unpacking the VMulti package failed (tar exit code " +
                                          std::to_wstring(exit_code) + L")"};
    }
}

void verify_catalog(const std::wstring &path) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();

    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const HWND no_ui = static_cast<HWND>(INVALID_HANDLE_VALUE);
    const LONG status = WinVerifyTrust(no_ui, &action, &data);

    std::wstring signer;
    if (status == ERROR_SUCCESS) {
        if (CRYPT_PROVIDER_DATA *provider = WTHelperProvDataFromStateData(data.hWVTStateData)) {
            if (CRYPT_PROVIDER_SGNR *signature = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0)) {
                if (CRYPT_PROVIDER_CERT *certificate = WTHelperGetProvCertFromChain(signature, 0)) {
                    wchar_t name[256] = {};
                    if (CertGetNameStringW(certificate->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256) > 1) {
                        signer = name;
                    }
                }
            }
        }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(no_ui, &action, &data);

    if (status != ERROR_SUCCESS) fail_code(L"Checking the driver catalog's signature", static_cast<DWORD>(status));
    if (signer != kCatalogSigner) {
        throw SetupError{kExitFailed, L"The driver catalog is signed by \"" + signer + L"\", not by \"" +
                                          kCatalogSigner + L"\", so it was not installed"};
    }
}

struct InstalledDevice {
    std::wstring instance_id;
    std::wstring driver_inf;  // the package's name in the driver store, such as oem88.inf
    bool needs_restart = false;
};

// Does what `devcon install vmulti.inf pentablet\hid` does: creates a root-enumerated device node with
// the hardware ID, then installs the driver package onto it.
InstalledDevice install_device(const std::wstring &inf) {
    GUID class_guid;
    wchar_t class_name[MAX_CLASS_NAME_LEN];
    if (!SetupDiGetINFClassW(inf.c_str(), &class_guid, class_name, MAX_CLASS_NAME_LEN, nullptr)) {
        fail_last_error(L"Reading the driver's INF file");
    }
    const HDEVINFO devices = SetupDiCreateDeviceInfoList(&class_guid, nullptr);
    if (devices == INVALID_HANDLE_VALUE) fail_last_error(L"Preparing the device");

    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    static const wchar_t hardware_ids[] = L"pentablet\\hid\0";  // a list of strings ending in an empty one
    bool registered = false;
    try {
        if (!SetupDiCreateDeviceInfoW(devices, class_name, &class_guid, nullptr, nullptr, DICD_GENERATE_ID, &device)) {
            fail_last_error(L"Creating the device");
        }
        if (!SetupDiSetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID,
                                               reinterpret_cast<const BYTE *>(hardware_ids), sizeof(hardware_ids))) {
            fail_last_error(L"Setting the device's hardware ID");
        }
        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devices, &device)) fail_last_error(L"Registering the device");
        registered = true;

        InstalledDevice installed;
        wchar_t instance_id[kMaxDeviceIdLength];
        if (!SetupDiGetDeviceInstanceIdW(devices, &device, instance_id, kMaxDeviceIdLength, nullptr)) {
            fail_last_error(L"Reading the device's instance ID");
        }
        installed.instance_id = instance_id;

        BOOL restart = FALSE;
        if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf.c_str(), INSTALLFLAG_FORCE, &restart)) {
            fail_last_error(L"Installing the driver");
        }
        installed.needs_restart = restart != FALSE;

        wchar_t driver_inf[MAX_PATH] = {};
        DEVPROPTYPE type = 0;
        if (SetupDiGetDevicePropertyW(devices, &device, &DEVPKEY_Device_DriverInfPath, &type,
                                      reinterpret_cast<BYTE *>(driver_inf), sizeof(driver_inf), nullptr, 0)) {
            installed.driver_inf = driver_inf;
        }
        SetupDiDestroyDeviceInfoList(devices);
        return installed;
    } catch (...) {
        // Leave no half-installed device behind.
        if (registered) SetupDiCallClassInstaller(DIF_REMOVE, devices, &device);
        SetupDiDestroyDeviceInfoList(devices);
        throw;
    }
}

void write_string(HKEY key, const wchar_t *name, const std::wstring &value) {
    const LSTATUS status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) fail_code(L"Recording the installation", static_cast<DWORD>(status));
}

void record_ownership(const InstalledDevice &installed) {
    HKEY key;
    const LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kOwnershipKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                           &key, nullptr);
    if (status != ERROR_SUCCESS) fail_code(L"Recording the installation", static_cast<DWORD>(status));
    try {
        write_string(key, L"DeviceInstanceId", installed.instance_id);
        write_string(key, L"DriverInf", installed.driver_inf);
    } catch (...) {
        RegCloseKey(key);
        throw;
    }
    RegCloseKey(key);
}

bool read_string(const wchar_t *name, std::wstring &value) {
    wchar_t buffer[512] = {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kOwnershipKey, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return false;
    }
    value = buffer;
    return true;
}

bool wait_until_working(DWORD timeout_ms) {
    for (DWORD waited = 0;; waited += 500) {
        if (vmulti_working()) return true;
        if (waited >= timeout_ms) return false;
        Sleep(500);
    }
}

int run_install() {
    if (!is_x64_pc()) {
        throw SetupError{kExitUnsupported,
                         L"The VMulti driver exists only for x64 Windows, and this PC is not x64 (an ARM PC, for example)."};
    }
    if (vmulti_working()) {
        say(L"VMulti is already installed and working. Nothing to do.");
        return kExitOk;
    }
    if (vmulti_device_exists()) {
        throw SetupError{kExitFailed,
                         L"A VMulti device (\"Pentablet HID\") exists but is not working. Check it in Device Manager, "
                         L"or remove it and run setup again."};
    }

    WorkFolder work;
    const std::wstring package = work.file(kPackageFile);
    const std::wstring bundled = program_folder() + L"\\" + kPackageFile;
    if (GetFileAttributesW(bundled.c_str()) != INVALID_FILE_ATTRIBUTES) {
        say(L"Using the VMulti package next to setup.");
        if (!CopyFileW(bundled.c_str(), package.c_str(), TRUE)) fail_last_error(L"Copying the VMulti package");
    } else {
        say(L"Downloading the VMulti package...");
        download(kPackageUrl, package);
    }

    const std::wstring hash = sha256_hex(package);
    if (hash != kPackageSha256) {
        throw SetupError{kExitFailed, L"The VMulti package does not match the expected SHA-256 hash, so it was not "
                                      L"installed.\n  expected " + std::wstring(kPackageSha256) + L"\n  found    " + hash};
    }
    say(L"Package hash verified.");

    unpack(package, work.path());
    verify_catalog(work.file(kCatalogFile));
    say(L"Microsoft's signature on the driver catalog verified. Installing...");

    const InstalledDevice installed = install_device(work.file(kInfFile));
    record_ownership(installed);
    if (installed.needs_restart) {
        say(L"VMulti is installed. Restart Windows to start it.");
        return kExitRebootRequired;
    }
    if (!wait_until_working(kStartTimeoutMs)) {
        throw SetupError{kExitFailed,
                         L"The driver was installed, but Windows did not start it. If Windows showed a notification "
                         L"that a driver can't load, Memory Integrity (Windows Security > Device security > Core "
                         L"isolation) blocked it. Run \"xva-setup /uninstall\" to remove it."};
    }
    say(L"VMulti is installed and working.");
    return kExitOk;
}

int run_uninstall() {
    std::wstring instance_id;
    std::wstring driver_inf;
    if (!read_string(L"DeviceInstanceId", instance_id)) {
        say(L"This setup did not install VMulti on this PC, so it is left as it is.");
        return kExitOk;
    }
    read_string(L"DriverInf", driver_inf);

    BOOL restart = FALSE;
    const HDEVINFO devices = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (devices == INVALID_HANDLE_VALUE) fail_last_error(L"Preparing to remove the device");
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    if (SetupDiOpenDeviceInfoW(devices, instance_id.c_str(), nullptr, 0, &device)) {
        if (!DiUninstallDevice(nullptr, devices, &device, 0, &restart)) {
            const DWORD error = GetLastError();
            SetupDiDestroyDeviceInfoList(devices);
            fail_code(L"Removing the VMulti device", error);
        }
        say(L"Removed the VMulti device.");
    } else {
        say(L"The VMulti device was already gone.");
    }
    SetupDiDestroyDeviceInfoList(devices);

    if (!driver_inf.empty()) {
        const std::wstring name = std::filesystem::path(driver_inf).filename().wstring();
        const BOOL removed = SetupUninstallOEMInfW(name.c_str(), 0, nullptr);
        const DWORD error = removed ? ERROR_SUCCESS : GetLastError();
        if (removed) {
            say(L"Removed the driver package from Windows' driver store.");
        } else if (error == ERROR_INF_IN_USE_BY_DEVICES) {
            say(L"Kept the driver package in Windows' driver store, because another device still uses it.");
        } else {
            say(L"Could not remove the driver package " + name + L": " + system_message(error));
        }
    }

    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kProductKey);
    if (restart) {
        say(L"Restart Windows to finish removing VMulti.");
        return kExitRebootRequired;
    }
    return kExitOk;
}

// When setup was started by double-clicking, its window would close before the result could be read.
void pause_if_own_window() {
    DWORD processes[2];
    if (GetConsoleProcessList(processes, 2) == 1) {
        say(L"\nPress Enter to close.");
        (void)getwchar();
    }
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    const bool uninstall =
        argc == 2 && (_wcsicmp(argv[1], L"/uninstall") == 0 || _wcsicmp(argv[1], L"--uninstall") == 0);
    int exit_code;
    if (argc > 1 && !uninstall) {
        say(L"Usage: xva-setup            install the VMulti driver XVA needs\n"
            L"       xva-setup /uninstall remove it again, if this setup installed it");
        exit_code = kExitFailed;
    } else {
        try {
            exit_code = uninstall ? run_uninstall() : run_install();
        } catch (const SetupError &error) {
            fwprintf(stderr, L"%ls\n", error.message.c_str());
            exit_code = error.exit_code;
        } catch (const std::exception &error) {
            fprintf(stderr, "%s\n", error.what());
            exit_code = kExitFailed;
        }
    }
    pause_if_own_window();
    return exit_code;
}
