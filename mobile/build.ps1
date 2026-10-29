[CmdletBinding()]
param(
    [string]$AndroidSdk = $(if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { $env:ANDROID_HOME }),
    [string]$DesktopExe = '../build/v2.3.0/Release/Chuan.exe'
)
$ErrorActionPreference = 'Stop'
$taskMobileRoot = $PSScriptRoot
$taskRoot = Split-Path -Parent $taskMobileRoot
if (-not $AndroidSdk -or -not (Test-Path -LiteralPath $AndroidSdk)) { throw '请通过 -AndroidSdk 指定含 Android 35 与 Build Tools 35.0.0 的 SDK。' }
$taskSdk = [IO.Path]::GetFullPath($AndroidSdk)
$taskTools = Join-Path $taskSdk 'build-tools/35.0.0'
$taskAndroidJar = Join-Path $taskSdk 'platforms/android-35/android.jar'
foreach ($taskTool in @('aapt2.exe','d8.bat','zipalign.exe','apksigner.bat')) { if (-not (Test-Path -LiteralPath (Join-Path $taskTools $taskTool))) { throw ('缺少 Android 构建工具：' + $taskTool) } }
if (-not [IO.Path]::IsPathRooted($DesktopExe)) { $DesktopExe = Join-Path $taskMobileRoot $DesktopExe }
$DesktopExe = [IO.Path]::GetFullPath($DesktopExe)
if (-not (Test-Path -LiteralPath $DesktopExe)) { throw '请先构建同版本 Windows 客户端，并通过 -DesktopExe 指定可执行文件。' }
$taskVersion = [regex]::Match((Get-Content -LiteralPath (Join-Path $taskRoot 'CMakeLists.txt') -Raw),'project\(Chuan\s+VERSION\s+(\d+\.\d+(?:\.\d+)?)').Groups[1].Value
$taskManifest = [xml](Get-Content -LiteralPath (Join-Path $taskMobileRoot 'src/main/AndroidManifest.xml') -Raw)
$taskAndroidVersion = $taskManifest.manifest.GetAttribute('versionName','http://schemas.android.com/apk/res/android')
$taskEngineVersion = [regex]::Match((Get-Content -LiteralPath (Join-Path $taskMobileRoot 'src/main/java/local/chuan/core/Engine.java') -Raw),'VERSION="([^"]+)"').Groups[1].Value
if (-not $taskVersion -or $taskAndroidVersion -ne $taskVersion -or $taskEngineVersion -ne $taskVersion -or (Get-Item -LiteralPath $DesktopExe).VersionInfo.ProductVersion -ne $taskVersion) { throw 'Android 源码、项目与 Windows 程序必须使用同一当前版本。' }
$taskHash = [Security.Cryptography.SHA256]::Create()
try { $taskCacheId = [BitConverter]::ToString($taskHash.ComputeHash([Text.Encoding]::UTF8.GetBytes($taskMobileRoot))).Replace('-','').Substring(0,12) } finally { $taskHash.Dispose() }
# AAPT2 on Windows requires an ASCII workspace. Only intermediate files use the temporary directory.
$taskWork = Join-Path ([IO.Path]::GetTempPath()) ('chuan-android-' + $taskCacheId)
$taskWork = [IO.Path]::GetFullPath($taskWork)
$taskTempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
if (-not $taskWork.StartsWith($taskTempRoot,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($taskWork) -ne ('chuan-android-' + $taskCacheId)) { throw 'Android 中间目录不属于本次构建范围。' }
if (Test-Path -LiteralPath $taskWork) { Remove-Item -LiteralPath $taskWork -Recurse -Force }
$taskOutputRoot = Join-Path $taskMobileRoot 'build/outputs'
New-Item -ItemType Directory -Path $taskOutputRoot -Force | Out-Null
foreach ($taskPart in @('assets/web/download','assets/native','res/drawable','generated','classes','dex','outputs')) { New-Item -ItemType Directory -Path (Join-Path $taskWork $taskPart) -Force | Out-Null }
Copy-Item -LiteralPath (Join-Path $taskMobileRoot 'src/main/AndroidManifest.xml') -Destination (Join-Path $taskWork 'AndroidManifest.xml') -Force
Copy-Item -Path (Join-Path $taskRoot 'src/web/*') -Destination (Join-Path $taskWork 'assets/web') -Force
Copy-Item -Path (Join-Path $taskMobileRoot 'src/main/assets/native/*') -Destination (Join-Path $taskWork 'assets/native') -Force
Copy-Item -Path (Join-Path $taskMobileRoot 'src/main/res/*') -Destination (Join-Path $taskWork 'res') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'src/assets/chuan.png') -Destination (Join-Path $taskWork 'res/drawable/chuan.png') -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'src/assets/chuan.svg') -Destination (Join-Path $taskWork 'assets/web/chuan.svg') -Force
Copy-Item -LiteralPath $DesktopExe -Destination (Join-Path $taskWork 'assets/web/download/Chuan.exe') -Force
$taskSprite = [regex]::Match((Get-Content -LiteralPath (Join-Path $taskRoot 'src/web/index.html') -Raw),'<svg class="symbols"[\s\S]*?</svg>').Value
if (-not $taskSprite) { throw '无法读取原有 Web 图标定义。' }
$taskHtml = (Get-Content -LiteralPath (Join-Path $taskMobileRoot 'src/main/assets/native/app.html') -Raw).Replace('<!-- ORIGINAL_ICONS -->',$taskSprite)
[IO.File]::WriteAllText((Join-Path $taskWork 'assets/native/app.html'),$taskHtml,[Text.UTF8Encoding]::new($false))
function Invoke-MobileTool([string]$Executable,[string[]]$Arguments) { & $Executable @Arguments; if ($LASTEXITCODE -ne 0) { throw ('Android 构建失败：' + [IO.Path]::GetFileName($Executable)) } }
Invoke-MobileTool (Join-Path $taskTools 'aapt2.exe') @('compile','--dir',(Join-Path $taskWork 'res'),'-o',(Join-Path $taskWork 'resources.zip'))
Invoke-MobileTool (Join-Path $taskTools 'aapt2.exe') @('link','-I',$taskAndroidJar,'--manifest',(Join-Path $taskWork 'AndroidManifest.xml'),'-A',(Join-Path $taskWork 'assets'),(Join-Path $taskWork 'resources.zip'),'--java',(Join-Path $taskWork 'generated'),'-o',(Join-Path $taskWork 'resources.apk'),'--min-sdk-version','26','--target-sdk-version','35')
$taskJavaFiles = @(& rg --files (Join-Path $taskMobileRoot 'src/main/java') (Join-Path $taskWork 'generated') -g '*.java')
$taskJavaArgs = @('-encoding','UTF-8','--release','8','-Xlint:-options','-classpath',$taskAndroidJar,'-d',(Join-Path $taskWork 'classes')) + $taskJavaFiles
$taskArgsFile = Join-Path $taskWork 'javac.args'
[IO.File]::WriteAllLines($taskArgsFile,($taskJavaArgs | ForEach-Object { '"' + $_.Replace('\','/') + '"' }),[Text.UTF8Encoding]::new($false))
Invoke-MobileTool 'javac.exe' @('@' + $taskArgsFile)
$taskClassFiles = @(& rg --files (Join-Path $taskWork 'classes') -g '*.class')
$taskDexArgs = @('--lib',$taskAndroidJar,'--min-api','26','--output',(Join-Path $taskWork 'dex')) + $taskClassFiles
$taskDexArgsFile = Join-Path $taskWork 'd8.args'
[IO.File]::WriteAllLines($taskDexArgsFile,($taskDexArgs | ForEach-Object { $_.Replace('\','/') }),[Text.UTF8Encoding]::new($false))
Invoke-MobileTool (Join-Path $taskTools 'd8.bat') @('@' + $taskDexArgsFile)
Copy-Item -LiteralPath (Join-Path $taskWork 'resources.apk') -Destination (Join-Path $taskWork 'unsigned.apk') -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem
$taskArchive = [IO.Compression.ZipFile]::Open((Join-Path $taskWork 'unsigned.apk'),[IO.Compression.ZipArchiveMode]::Update)
try {
    # AAPT2 on Windows can emit backslashes in asset entry names; Android AssetManager requires '/'.
    foreach ($taskEntry in @($taskArchive.Entries | Where-Object { $_.FullName.Contains('\') })) {
        $taskName = $taskEntry.FullName.Replace('\','/')
        $taskMemory = [IO.MemoryStream]::new()
        $taskInput = $taskEntry.Open()
        try { $taskInput.CopyTo($taskMemory) } finally { $taskInput.Dispose() }
        $taskEntry.Delete()
        $taskNewEntry = $taskArchive.CreateEntry($taskName,[IO.Compression.CompressionLevel]::Optimal)
        $taskStream = $taskNewEntry.Open()
        try { $taskMemory.Position=0; $taskMemory.CopyTo($taskStream) } finally { $taskStream.Dispose(); $taskMemory.Dispose() }
    }
    foreach ($taskDex in Get-ChildItem -LiteralPath (Join-Path $taskWork 'dex') -Filter '*.dex') { [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($taskArchive,$taskDex.FullName,$taskDex.Name,[IO.Compression.CompressionLevel]::Optimal) | Out-Null }
} finally { $taskArchive.Dispose() }
Invoke-MobileTool (Join-Path $taskTools 'zipalign.exe') @('-f','-p','4',(Join-Path $taskWork 'unsigned.apk'),(Join-Path $taskWork 'aligned.apk'))
$taskSignDir = Join-Path $taskMobileRoot '.signing'
New-Item -ItemType Directory -Path $taskSignDir -Force | Out-Null
$taskKey = Join-Path $taskSignDir 'chuan-release.p12'
$taskPasswordFile = Join-Path $taskSignDir 'password.txt'
if (-not (Test-Path -LiteralPath $taskKey)) {
    $taskSecretBytes = New-Object byte[] 32
    $taskRandom = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $taskRandom.GetBytes($taskSecretBytes) } finally { $taskRandom.Dispose() }
    [IO.File]::WriteAllText($taskPasswordFile,[Convert]::ToBase64String($taskSecretBytes),[Text.UTF8Encoding]::new($false))
    Invoke-MobileTool 'keytool.exe' @('-genkeypair','-keystore',$taskKey,'-storetype','PKCS12','-storepass:file',$taskPasswordFile,'-alias','chuan-release','-keyalg','RSA','-keysize','3072','-validity','10000','-dname','CN=Chuan Android, O=Chuan')
}
if (-not (Test-Path -LiteralPath $taskPasswordFile)) { throw 'Android 发布签名密钥的密码文件缺失。' }
$taskOutput = Join-Path $taskWork ('outputs/Chuan-' + $taskVersion + '-android.apk')
Invoke-MobileTool (Join-Path $taskTools 'apksigner.bat') @('sign','--ks',$taskKey,'--ks-key-alias','chuan-release','--ks-pass',('file:'+$taskPasswordFile),'--out',$taskOutput,(Join-Path $taskWork 'aligned.apk'))
Invoke-MobileTool (Join-Path $taskTools 'apksigner.bat') @('verify','--verbose',$taskOutput)
Invoke-MobileTool (Join-Path $taskTools 'zipalign.exe') @('-c','4',$taskOutput)
$taskFinalPackage = Join-Path $taskOutputRoot ([IO.Path]::GetFileName($taskOutput))
Copy-Item -LiteralPath $taskOutput -Destination $taskFinalPackage -Force
[pscustomobject]@{ AndroidPackage=$taskFinalPackage; Version=$taskVersion; SigningKey=$taskKey; AndroidSdk=$taskSdk; IntermediateDirectory=$taskWork }
