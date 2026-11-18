using Microsoft.Win32;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;

namespace PhoneCam;

public static class CameraInstallation
{
    public const string Clsid = "{E596E156-A65E-489A-A4FB-F2F4B0D23E01}";
    // Windows 10 uses a DirectShow video input filter instead of MFCreateVirtualCamera.
    public const string DirectShowClsid = "{3791D0FD-59B4-4DB9-AF2F-2847FCB5CD06}";
    const string VideoInputCategory = "{860BB310-5D01-11D0-BD3B-00A0C911CE86}";
    // Use the project version so component updates cannot retain an obsolete installation path.
    public static readonly string Version = typeof(CameraInstallation).Assembly.GetName().Version!.ToString(3);
    static readonly SecurityIdentifier Administrators = new(WellKnownSidType.BuiltinAdministratorsSid, null);
    static readonly SecurityIdentifier SystemAccount = new(WellKnownSidType.LocalSystemSid, null);
    static readonly Lazy<byte[]> EmbeddedDll = new(() => ReadPayload("PCam.Camera.dll"));
    // 32-bit twin of the filter: 32-bit consumers (ManyCam and other older
    // DirectShow apps) enumerate and load only the 32-bit registry view.
    static readonly Lazy<byte[]> EmbeddedDll32 = new(() => ReadPayload("PCam.Camera32.dll"));
    static readonly Lazy<string> Hash64 = new(() => Convert.ToHexString(SHA256.HashData(EmbeddedDll.Value)));
    static readonly Lazy<string> Hash32 = new(() => Convert.ToHexString(SHA256.HashData(EmbeddedDll32.Value)));
    // Hash both payloads so a change to either selects a fresh directory.
    static readonly Lazy<string> PayloadHash = new(() => Convert.ToHexString(SHA256.HashData(EmbeddedDll.Value.Concat(EmbeddedDll32.Value).ToArray())));
    public static string Sid => WindowsIdentity.GetCurrent().User!.Value;

    public static string FramesDirectory(string sid)
    {
        // SID is command-line input to an elevated helper: never accept it as a path fragment.
        var identity = new SecurityIdentifier(sid);
        if (!identity.IsAccountSid() || identity.Value != sid) throw new ArgumentException("用户标识无效");
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "PCam", "Frames", identity.Value);
    }
    public static string FrameFile => Path.Combine(FramesDirectory(Sid), "frames.bin");
    static byte[] ReadPayload(string name)
    {
        using var stream = typeof(CameraInstallation).Assembly.GetManifestResourceStream(name)
            ?? throw new InvalidOperationException("缺少虚拟摄像头组件");
        using var output = new MemoryStream();
        stream.CopyTo(output);
        return output.ToArray();
    }
    // Both DLLs share one installation directory so the 32-bit copy can never
    // drift from the 64-bit copy it was built alongside.
    public static string InstallDirectory => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
        "PCam", "VirtualCamera", Version, PayloadHash.Value[..12]);
    public static string InstalledPath => Path.Combine(InstallDirectory, "PCamCamera.dll");
    public static string InstalledPath32 => Path.Combine(InstallDirectory, "PCamCamera32.dll");

    public static void RejectReparsePoints(string path)
    {
        for (string? current = Path.GetFullPath(path); current != null; current = Path.GetDirectoryName(current))
        {
            try
            {
                // Path.Exists follows links and may report false for a dangling link.
                if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("安装路径含目录联接或符号链接，请检查 PCam 目录");
            }
            catch (FileNotFoundException) { }
            catch (DirectoryNotFoundException) { }
        }
    }
    public static bool IsInstalled()
    {
        try
        {
            RejectReparsePoints(InstalledPath);
            RejectReparsePoints(FramesDirectory(Sid));
            if(!HasSafeFrameDirectory())return false;
            using var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Classes\CLSID\" + Clsid + @"\InprocServer32");
            if (!string.Equals(key?.GetValue(null) as string, InstalledPath, StringComparison.OrdinalIgnoreCase)
                || !Directory.Exists(FramesDirectory(Sid))) return false;
            using var directShow = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Classes\CLSID\" + DirectShowClsid + @"\InprocServer32");
            if (!string.Equals(directShow?.GetValue(null) as string, InstalledPath, StringComparison.OrdinalIgnoreCase)) return false;
            using var device = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Classes\CLSID\" + VideoInputCategory + @"\Instance\" + DirectShowClsid);
            if (device == null) return false;
            // The 32-bit twin must be registered in the 32-bit registry view as well.
            using var view32 = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32);
            using var directShow32 = view32.OpenSubKey(@"SOFTWARE\Classes\CLSID\" + DirectShowClsid + @"\InprocServer32");
            if (!string.Equals(directShow32?.GetValue(null) as string, InstalledPath32, StringComparison.OrdinalIgnoreCase)) return false;
            using var device32 = view32.OpenSubKey(@"SOFTWARE\Classes\CLSID\" + VideoInputCategory + @"\Instance\" + DirectShowClsid);
            if (device32 == null) return false;
            // Check the actual DLLs too; directory name/registration alone do not prove integrity.
            using (var file = File.OpenRead(InstalledPath))
                if (Convert.ToHexString(SHA256.HashData(file)) != Hash64.Value) return false;
            using (var file = File.OpenRead(InstalledPath32))
                if (Convert.ToHexString(SHA256.HashData(file)) != Hash32.Value) return false;
            return true;
        }
        catch { return false; }
    }
    static bool HasSafeFrameDirectory()
    {
        var security=new DirectoryInfo(FramesDirectory(Sid)).GetAccessControl(AccessControlSections.Access|AccessControlSections.Owner);
        if(!security.AreAccessRulesProtected || !Administrators.Equals(security.GetOwner(typeof(SecurityIdentifier))))return false;
        const FileSystemRights forbidden=FileSystemRights.Delete|FileSystemRights.WriteAttributes|FileSystemRights.WriteExtendedAttributes|
            FileSystemRights.ChangePermissions|FileSystemRights.TakeOwnership|FileSystemRights.CreateDirectories;
        foreach(FileSystemAccessRule rule in security.GetAccessRules(true,false,typeof(SecurityIdentifier)))
            if(rule.IdentityReference.Value==Sid && rule.AccessControlType==AccessControlType.Allow &&
                (rule.PropagationFlags&PropagationFlags.InheritOnly)==0 && (rule.FileSystemRights&forbidden)!=0)return false;
        return true;
    }
    static DirectorySecurity DirectoryRules(SecurityIdentifier? user = null)
    {
        var rules = new DirectorySecurity();
        rules.SetAccessRuleProtection(true, false);
        rules.SetOwner(Administrators);
        void Add(SecurityIdentifier identity, FileSystemRights rights) => rules.AddAccessRule(new(identity, rights,
            InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
        Add(SystemAccount, FileSystemRights.FullControl);
        Add(Administrators, FileSystemRights.FullControl);
        if (user == null) Add(new(WellKnownSidType.BuiltinUsersSid, null), FileSystemRights.ReadAndExecute);
        else
        {
            // The user can create frame files, but cannot replace/reparse the SID directory itself.
            rules.AddAccessRule(new(user,FileSystemRights.ReadAndExecute|FileSystemRights.CreateFiles,
                InheritanceFlags.None,PropagationFlags.None,AccessControlType.Allow));
            rules.AddAccessRule(new(user,FileSystemRights.Modify,
                InheritanceFlags.ObjectInherit,PropagationFlags.InheritOnly,AccessControlType.Allow));
            Add(new(WellKnownSidType.LocalServiceSid, null), FileSystemRights.ReadAndExecute);
        }
        return rules;
    }
    static void SecureDirectory(string path, DirectorySecurity rules)
    {
        RejectReparsePoints(path);
        var directory = new DirectoryInfo(path);
        // Create with its restrictive ACL from the outset. Refuse pre-planted user-owned folders.
        if (!directory.Exists) directory.Create(rules);
        InstallDirectorySecurity.Apply(path,rules);

    }
    static void SecureTree(string parent, params string[] names)
    {
        foreach (string name in names)
        {
            parent = Path.Combine(parent, name);
            SecureDirectory(parent, DirectoryRules());
        }
    }
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int Register();
    public static void Install(string sid)
    {
        string frames = FramesDirectory(sid);
        var principal = new WindowsPrincipal(WindowsIdentity.GetCurrent());
        if (!principal.IsInRole(WindowsBuiltInRole.Administrator)) throw new UnauthorizedAccessException("注册摄像头组件需要管理员权限");
        SecureTree(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "PCam", "VirtualCamera", Version, PayloadHash.Value[..12]);
        SecureTree(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "PCam", "Frames");
        SecureDirectory(frames, DirectoryRules(new(sid)));
        RejectReparsePoints(InstalledPath);
        WritePayload(InstalledPath, EmbeddedDll.Value, Hash64.Value);
        WritePayload(InstalledPath32, EmbeddedDll32.Value, Hash32.Value);
        nint dll = NativeLibrary.Load(InstalledPath);
        try { Marshal.ThrowExceptionForHR(Marshal.GetDelegateForFunctionPointer<Register>(NativeLibrary.GetExport(dll, "PcRegister"))()); }
        finally { NativeLibrary.Free(dll); }
        Register32Bit();
    }
    static void WritePayload(string path, byte[] payload, string hash)
    {
        if (!File.Exists(path))
        {
            using var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
            file.Write(payload);
        }
        using var read = File.OpenRead(path);
        if (Convert.ToHexString(SHA256.HashData(read)) != hash) throw new IOException("摄像头组件校验失败");
    }
    static void Register32Bit()
    {
        // SysWOW64\regsvr32 is a 32-bit process, so the filter's COM and filter
        // mapper entries land in the 32-bit registry view where 32-bit
        // consumers enumerate devices. The 64-bit registration above covers
        // 64-bit consumers.
        var regsvr32 = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.SystemX86), "regsvr32.exe");
        if (!File.Exists(regsvr32)) throw new IOException("缺少 32 位组件注册工具");
        var start = new ProcessStartInfo(regsvr32) { UseShellExecute = false, CreateNoWindow = true };
        start.ArgumentList.Add("/s");
        start.ArgumentList.Add(InstalledPath32);
        using var process = Process.Start(start) ?? throw new IOException("32 位摄像头组件注册未启动");
        process.WaitForExit();
        if (process.ExitCode != 0) throw new IOException("32 位摄像头组件注册失败");
    }
    public static async Task EnsureInstalled()
    {
        if (IsInstalled()) return;
        var start = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden };
        start.ArgumentList.Add("--install-camera");
        start.ArgumentList.Add(Sid);
        using var process = Process.Start(start) ?? throw new IOException("摄像头安装未启动");
        await process.WaitForExitAsync();
        if (process.ExitCode != 0 || !IsInstalled()) throw new IOException("虚拟摄像头组件安装未完成，请重新打开 PCam 并允许安装");
    }
}
