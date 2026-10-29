[CmdletBinding()]
param([Parameter(Mandatory)][string]$OldExe,[Parameter(Mandatory)][string]$NewExe,[Parameter(Mandatory)][string]$OutputDirectory,[string]$CandidateExe,[string]$HelperExe,[switch]$ExpectRollback)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $taskRoot){throw '测试目录必须为新目录。'}
New-Item -ItemType Directory -Path $taskRoot | Out-Null
& icacls.exe $taskRoot /setintegritylevel '(OI)(CI)M' | Out-Null
if($LASTEXITCODE -ne 0){throw '无法为隔离测试目录设置中完整性标签。'}
$taskStage=Join-Path $taskRoot 'stage';$taskInstalled=Join-Path $taskRoot 'installed';$taskData=Join-Path $taskRoot 'data'
foreach($taskDirectory in @($taskStage,$taskInstalled,$taskData)){New-Item -ItemType Directory -Path $taskDirectory | Out-Null}
$taskCandidate=Join-Path $taskStage 'Chuan.exe';$taskTarget=Join-Path $taskInstalled 'Chuan.exe';$taskHelper=Join-Path $taskStage 'helper.exe'
if(-not $CandidateExe){$CandidateExe=$NewExe}
Copy-Item -LiteralPath $CandidateExe -Destination $taskCandidate
if(-not $HelperExe){$HelperExe=$NewExe}
Copy-Item -LiteralPath $HelperExe -Destination $taskHelper
Copy-Item -LiteralPath $OldExe -Destination $taskTarget
$taskOldHash=(Get-FileHash -LiteralPath $taskTarget -Algorithm SHA256).Hash.ToLowerInvariant()
$taskNewHash=(Get-FileHash -LiteralPath $taskCandidate -Algorithm SHA256).Hash.ToLowerInvariant()
if($taskOldHash -eq $taskNewHash){throw '测试需要内容不同的两个程序。'}
$taskListener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$taskListener.Start();$taskPort=$taskListener.LocalEndpoint.Port;$taskListener.Stop()
$taskTransaction=[Guid]::NewGuid().ToString('B')
[IO.File]::WriteAllText((Join-Path $taskData 'settings.ini'),"[Chuan]`r`nName=Updater restart check`r`nId=$([Guid]::NewGuid().ToString())`r`nPort=$taskPort`r`nReceiveDir=$(Join-Path $taskData 'received')`r`n",[Text.Encoding]::Unicode)
$taskCandidateVersion='V'+(Get-Item -LiteralPath $taskCandidate).VersionInfo.ProductVersion
$taskPlanValues=@($taskCandidate,$taskTarget,$taskData,$taskNewHash,$taskCandidateVersion,$taskTransaction,'')
$taskLines=$taskPlanValues|ForEach-Object {'"'+$_.Replace('\','\\').Replace('"','\"')+'"'}
$taskPlan=Join-Path $taskStage 'plan';[IO.File]::WriteAllLines($taskPlan,$taskLines,[Text.UTF8Encoding]::new($false))
$taskOriginal=$null;$taskCoordinator=$null;$taskRestarted=@()
try{
    $taskOriginal=Start-Process -FilePath $taskTarget -ArgumentList @('--headless','--run-seconds','2','--data-dir',('"'+$taskData+'"')) -WindowStyle Hidden -PassThru
    $taskCoordinator=Start-Process -FilePath $taskHelper -ArgumentList @('--apply-update',('"'+$taskPlan+'"'),'--parent',$taskOriginal.Id) -WindowStyle Hidden -PassThru
    if($ExpectRollback){
        $taskDeadline=[DateTime]::UtcNow.AddSeconds(30)
        do{
            Start-Sleep -Milliseconds 200
            $taskRestarted=@(Get-CimInstance Win32_Process -Filter "Name='Chuan.exe'" | Where-Object {$_.ExecutablePath -eq $taskTarget -and $_.ProcessId -ne $taskOriginal.Id})
        }while($taskRestarted.Count -eq 0 -and [DateTime]::UtcNow -lt $taskDeadline)
        if($taskRestarted.Count -eq 0){throw '启动失败后未自动恢复运行旧版。'}
        # The coordinator now displays the expected failure notice; finally closes this test-only process.
    }else{
        if(-not $taskCoordinator.WaitForExit(30000)){throw '更新进程未能在 30 秒内完成。'}
        if($taskCoordinator.ExitCode -ne 0){throw ('更新进程返回 '+$taskCoordinator.ExitCode)}
    }
    $taskExpectedHash=if($ExpectRollback){$taskOldHash}else{$taskNewHash}
    if((Get-FileHash -LiteralPath $taskTarget -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskExpectedHash){throw '主程序替换或恢复结果不一致。'}
    $taskBackup=Join-Path $taskInstalled ('.Chuan-backup-'+$taskTransaction+'.exe')
    if(-not $ExpectRollback -and (Get-FileHash -LiteralPath $taskBackup -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskOldHash){throw '旧程序备份不一致。'}
    $taskRestarted=@(Get-CimInstance Win32_Process -Filter "Name='Chuan.exe'" | Where-Object ExecutablePath -EQ $taskTarget)
    if($taskRestarted.Count -ne 1){throw '新版程序未重新运行。'}
    if(-not (Test-Path -LiteralPath (Join-Path $taskData 'settings.ini'))){throw '用户配置未保留。'}
    if($ExpectRollback){Write-Output 'PASS failed startup acknowledgement, automatic rollback, old-version restart and preserved configuration'}
    else{Write-Output 'PASS running executable exit, atomic replacement, old-file backup, startup acknowledgement, restart and preserved configuration'}
}finally{
    foreach($taskProcess in $taskRestarted){$taskNative=Get-Process -Id $taskProcess.ProcessId -ErrorAction SilentlyContinue;if($taskNative){$null=$taskNative.CloseMainWindow();if(-not $taskNative.WaitForExit(5000)){Stop-Process -Id $taskNative.Id -Force}}}
    if($taskOriginal -and -not $taskOriginal.HasExited){Stop-Process -Id $taskOriginal.Id -Force}
    if($taskCoordinator -and -not $taskCoordinator.HasExited){Stop-Process -Id $taskCoordinator.Id -Force}
}
