[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build',
    [ValidateSet('Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [switch]$VersionedRelease,
    [string]$AndroidApk,
    [string]$AndroidSdk
)

$ErrorActionPreference = 'Stop'
$taskProjectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $taskProjectRoot $BuildDirectory
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$taskSourceExe = Join-Path $BuildDirectory ($Configuration + '\Chuan.exe')
$taskVersionMatch = [regex]::Match((Get-Content -LiteralPath (Join-Path $taskProjectRoot 'CMakeLists.txt') -Raw),
                                  'project\(Chuan\s+VERSION\s+(\d+\.\d+(?:\.\d+)?)\s')
if (-not $taskVersionMatch.Success) { throw '无法读取当前软件版本号。' }
$taskVersion = $taskVersionMatch.Groups[1].Value
if (-not (Test-Path -LiteralPath $taskSourceExe -PathType Leaf)) { throw '请先完成指定目录中的 Release 构建。' }
if ((Get-Item -LiteralPath $taskSourceExe).VersionInfo.ProductVersion -ne $taskVersion) {
    throw '可执行文件版本与项目当前版本不一致。'
}
$taskApkVerified = $false
if ($AndroidApk) {
    $AndroidApk = [IO.Path]::GetFullPath($AndroidApk)
    if (-not (Test-Path -LiteralPath $AndroidApk -PathType Leaf)) { throw 'Android 安装包不存在。' }
    if (-not $AndroidSdk) { throw '验证 Android 安装包需要通过 -AndroidSdk 指定 Android SDK。' }
    $taskAndroidTools = Join-Path ([IO.Path]::GetFullPath($AndroidSdk)) 'build-tools/35.0.0'
    # Native Android tools use an ASCII temporary path, including when the project folder contains Chinese characters.
    $taskApkCheckPath = Join-Path ([IO.Path]::GetTempPath()) ('chuan-package-' + [Guid]::NewGuid().ToString('N') + '.apk')
    Copy-Item -LiteralPath $AndroidApk -Destination $taskApkCheckPath
    & (Join-Path $taskAndroidTools 'apksigner.bat') verify --verbose $taskApkCheckPath
    if ($LASTEXITCODE -ne 0) { throw 'Android APK 签名验证失败。' }
    $taskBadging = & (Join-Path $taskAndroidTools 'aapt2.exe') dump badging $taskApkCheckPath
    if ($LASTEXITCODE -ne 0 -or -not ($taskBadging -match "name='local.chuan.app'") -or -not ($taskBadging -match ("versionName='" + [regex]::Escape($taskVersion) + "'"))) { throw 'Android 安装包标识或版本与桌面端不一致。' }
    $taskApkVerified = $true
}

if (-not ('ChuanPackageSecurity' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class ChuanPackageSecurity {
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern uint GetNamedSecurityInfoW(string name, uint type, uint info,
        out IntPtr owner, out IntPtr group, out IntPtr dacl, out IntPtr sacl, out IntPtr descriptor);
    [DllImport("advapi32.dll", SetLastError=true)]
    static extern bool GetAce(IntPtr acl, uint index, out IntPtr ace);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthority(IntPtr sid, uint index);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr memory);
    public static uint FileIntegrity(string path) {
        IntPtr owner, group, dacl, sacl, descriptor;
        uint error = GetNamedSecurityInfoW(path, 1, 0x10, out owner, out group, out dacl, out sacl, out descriptor);
        if (error != 0) throw new Win32Exception((int)error);
        try {
            if (sacl != IntPtr.Zero) {
                uint count = (ushort)Marshal.ReadInt16(sacl, 4);
                for (uint i = 0; i < count; ++i) {
                    IntPtr ace;
                    if (!GetAce(sacl, i, out ace)) throw new Win32Exception();
                    if (Marshal.ReadByte(ace) != 0x11) continue;
                    IntPtr sid = IntPtr.Add(ace, 8);
                    byte parts = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
                    return (uint)Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(parts - 1)));
                }
            }
            return 8192;
        } finally { LocalFree(descriptor); }
    }
}
'@
}

function Set-PackageMediumIntegrity([string]$Path, [bool]$Directory) {
    $taskLabel = if ($Directory) { '(OI)(CI)M' } else { 'M' }
    $taskAclOutput = & icacls.exe $Path /setintegritylevel $taskLabel 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw ('完整性标签校正失败。请从普通用户 PowerShell 运行打包脚本。' + [Environment]::NewLine + ($taskAclOutput -join [Environment]::NewLine))
    }
    if ([ChuanPackageSecurity]::FileIntegrity($Path) -ne 8192) { throw '发布文件必须具有中完整性级别。' }
}

function Get-StreamSha256([IO.Stream]$Stream) {
    $taskAlgorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($taskAlgorithm.ComputeHash($Stream)).Replace('-', '') }
    finally { $taskAlgorithm.Dispose() }
}

$taskChinaTime = [TimeZoneInfo]::ConvertTimeBySystemTimeZoneId([DateTime]::UtcNow, 'China Standard Time')
$taskStamp = $taskChinaTime.ToString('yyyyMMdd-HHmmss')
$taskReleaseRoot = if ($VersionedRelease) { Join-Path $taskProjectRoot ('__release_packages__\' + $taskVersion) } else { Join-Path $taskProjectRoot '__release_packages__\临时' }
$taskPackageDir = Join-Path $taskReleaseRoot $taskStamp
if (Test-Path -LiteralPath $taskPackageDir) { throw '同一时间的交付目录已存在；已有交付包不覆盖。' }
New-Item -ItemType Directory -Path $taskPackageDir | Out-Null
# 仅校正本次新建的交付目录，避免继承构建工作区的低完整性标签。
Set-PackageMediumIntegrity $taskPackageDir $true
$taskPackageExe = Join-Path $taskPackageDir 'Chuan.exe'
$taskPackageReadme = Join-Path $taskPackageDir 'README.md'
Copy-Item -LiteralPath $taskSourceExe -Destination $taskPackageExe
Copy-Item -LiteralPath (Join-Path $taskProjectRoot 'README.md') -Destination $taskPackageReadme
Set-PackageMediumIntegrity $taskPackageExe $false
$taskSourceHash = (Get-FileHash -LiteralPath $taskSourceExe -Algorithm SHA256).Hash
$taskExpectedHashes = @{
    'Chuan.exe' = $taskSourceHash
    'README.md' = (Get-FileHash -LiteralPath $taskPackageReadme -Algorithm SHA256).Hash
}
if ($AndroidApk) {
    $taskApkName = 'Chuan-' + $taskVersion + '-android.apk'
    $taskPackageApk = Join-Path $taskPackageDir $taskApkName
    Copy-Item -LiteralPath $AndroidApk -Destination $taskPackageApk
    $taskExpectedHashes[$taskApkName] = (Get-FileHash -LiteralPath $AndroidApk -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $taskPackageApk -Algorithm SHA256).Hash -ne $taskExpectedHashes[$taskApkName]) { throw 'Android 安装包复制完整性检查失败。' }
}
if ((Get-FileHash -LiteralPath $taskPackageExe -Algorithm SHA256).Hash -ne $taskSourceHash) {
    throw '可执行文件复制完整性检查失败。'
}
$taskSignature = Get-AuthenticodeSignature -LiteralPath $taskPackageExe
if ($taskSignature.Status -notin @('Valid', 'NotSigned')) { throw ('发行文件签名验证失败：' + $taskSignature.Status) }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$taskZipPath = Join-Path $taskPackageDir ('Chuan-' + $taskVersion + '-windows-x64.zip')
$taskZip = [IO.Compression.ZipFile]::Open($taskZipPath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($taskName in $taskExpectedHashes.Keys) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($taskZip, (Join-Path $taskPackageDir $taskName), $taskName, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $taskZip.Dispose() }
$taskZip = [IO.Compression.ZipFile]::OpenRead($taskZipPath)
try {
    if ($taskZip.Entries.Count -ne $taskExpectedHashes.Count -or -not $taskZip.GetEntry('Chuan.exe') -or -not $taskZip.GetEntry('README.md')) {
        throw 'ZIP 交付文件清单检查失败。'
    }
    foreach ($taskName in $taskExpectedHashes.Keys) {
        $taskEntry = $taskZip.GetEntry($taskName).Open()
        try { if ((Get-StreamSha256 $taskEntry) -ne $taskExpectedHashes[$taskName]) { throw ('ZIP 内文件完整性检查失败：' + $taskName) } }
        finally { $taskEntry.Dispose() }
    }
} finally { $taskZip.Dispose() }

# 解压检查使用构建目录，不向交付目录写入日志、测试数据或校验文件。
$taskQaDir = Join-Path $BuildDirectory ('package-qa\' + $taskStamp)
New-Item -ItemType Directory -Path $taskQaDir | Out-Null
Set-PackageMediumIntegrity $taskQaDir $true
[IO.Compression.ZipFile]::ExtractToDirectory($taskZipPath, $taskQaDir)
$taskExtractedExe = Join-Path $taskQaDir 'Chuan.exe'
if ([ChuanPackageSecurity]::FileIntegrity($taskExtractedExe) -ne 8192 -or
    (Get-FileHash -LiteralPath $taskExtractedExe -Algorithm SHA256).Hash -ne $taskSourceHash) {
    throw 'ZIP 解压后的完整性标签或文件内容检查失败。'
}
foreach ($taskName in $taskExpectedHashes.Keys) {
    if ((Get-FileHash -LiteralPath (Join-Path $taskQaDir $taskName) -Algorithm SHA256).Hash -ne $taskExpectedHashes[$taskName]) { throw ('ZIP 解压后的文件完整性检查失败：' + $taskName) }
}
[pscustomobject]@{
    ReleaseDirectory = $taskPackageDir
    Version = $taskVersion
    ExecutableIntegrityLevel = [ChuanPackageSecurity]::FileIntegrity($taskPackageExe)
    SignatureStatus = $taskSignature.Status.ToString()
    ArchiveVerified = $true
    AndroidSignatureVerified = $taskApkVerified
}
