# Copyright (c) 2026, Falcor project contributors.
# SPDX-License-Identifier: BSD-3-Clause
# 使用本机 Conda gs_env，并让 Falcor 构建匹配该环境的 Python。
[CmdletBinding()]
param(
    [ValidateSet('smoke', 'restir2020', 'paper', 'all')][string]$Suite = 'paper',
    [ValidateSet('smoke', 'preview', 'paper')][string]$Profile = 'preview',
    [string]$BuildDir = 'build/clion-release',
    [string]$AssetsRoot,
    [string]$Output,
    [string[]]$Scenes,
    [ValidateSet('d3d12', 'vulkan')][string]$DeviceType = 'd3d12',
    [int]$Width = 0,
    [int]$Height = 0,
    [int]$Frames = 0,
    [int]$Warmup = 0,
    [int]$MyMaxBounces = 0,
    [string]$RTXDIOptions,
    [ValidateSet('path-tracer', 'rtxdi-pass')][string]$BaselineRenderer = 'path-tracer',
    [switch]$List,
    [switch]$BuildOnly,
    [switch]$SkipBuild,
    [switch]$Html,
    [switch]$RequireComplete
)
$ErrorActionPreference = 'Stop'
$taskRepository = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $taskRepository
try {
    $env:PYTHONIOENCODING = 'utf-8'
    $taskEntry = Join-Path $taskRepository 'scripts/python/my_path_tracer/benchmark_suite.py'
    $taskArguments = @($taskEntry, '--suite', $Suite, '--profile', $Profile, '--device-type', $DeviceType, '--my-max-bounces', $MyMaxBounces, '--baseline-renderer', $BaselineRenderer)
    if ($AssetsRoot) { $taskArguments += @('--assets-root', $AssetsRoot) }
    if ($Output) { $taskArguments += @('--output', $Output) }
    if ($Scenes) { $taskArguments += @('--scenes') + @($Scenes | ForEach-Object { $_ -split ',' }) }
    if ($Width) { $taskArguments += @('--width', $Width) }
    if ($Height) { $taskArguments += @('--height', $Height) }
    if ($Frames) { $taskArguments += @('--frames', $Frames) }
    if ($Warmup) { $taskArguments += @('--warmup', $Warmup) }
    if ($RTXDIOptions) { $taskArguments += @('--rtxdi-options', $RTXDIOptions) }
    if ($RequireComplete) { $taskArguments += '--require-complete' }
    if ($Html) { $taskArguments += '--html' }
    $taskConda = if ($env:CONDA_EXE -and (Test-Path -LiteralPath $env:CONDA_EXE)) {
        $env:CONDA_EXE
    } else {
        (Get-Command conda.exe, conda.bat -ErrorAction SilentlyContinue | Select-Object -First 1).Source
    }
    if (-not $taskConda) { throw '找不到 Conda，请从已配置 Conda 的终端运行。' }
    $taskPythonInfo = & $taskConda run -n gs_env python -c "import json,sys; print(json.dumps(dict(executable=sys.executable,prefix=sys.prefix,version='.'.join(map(str,sys.version_info[:2])))))"
    if ($LASTEXITCODE -ne 0) { throw '无法运行本机 Conda gs_env。' }
    $taskPythonInfo = ($taskPythonInfo -join "`n") | ConvertFrom-Json
    $taskPython = $taskPythonInfo.executable
    if ($List) {
        & $taskConda run --no-capture-output -n gs_env python @taskArguments --list
        exit $LASTEXITCODE
    }
    $taskRequestedBuild = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $taskRepository $BuildDir }
    $taskBuildPath = [IO.Path]::GetFullPath($taskRequestedBuild)
    if (-not (Test-Path -LiteralPath (Join-Path $taskBuildPath 'CMakeCache.txt'))) { throw "请先配置 Falcor CMake 构建目录: $taskBuildPath" }
    $taskCache = Get-Content -LiteralPath (Join-Path $taskBuildPath 'CMakeCache.txt') -Raw
    $taskPythonCache = [regex]::Match($taskCache, '(?m)^Python_EXECUTABLE:FILEPATH=([^\r\n]+)').Groups[1].Value
    $taskMatchesPython = $taskCache -match '(?m)^FALCOR_USE_SYSTEM_PYTHON:BOOL=ON\r?$' -and
        $taskPythonCache.Replace('\', '/') -eq $taskPython.Replace('\', '/')
    if ($SkipBuild -and -not $taskMatchesPython) { throw '构建尚未匹配 gs_env，请先运行 tools\run_my_path_tracer_benchmark.bat -BuildOnly。' }
    if (-not $SkipBuild) {
        $taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $taskInstall = & $taskVswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $taskInstall) { throw '构建需要 Visual Studio 2022 C++ Build Tools。' }
        $taskVcvars = Join-Path $taskInstall 'VC/Auxiliary/Build/vcvarsall.bat'
        # cmd 只用于导入 MSVC 的进程环境，不执行文件修改。
        $taskEnvironment = & $env:ComSpec /d /c "call `"$taskVcvars`" x64 >nul && set"
        if ($LASTEXITCODE -ne 0) { throw '无法初始化 MSVC 2022 环境。' }
        foreach ($taskLine in $taskEnvironment) {
            $taskSeparator = $taskLine.IndexOf('=')
            if ($taskSeparator -gt 0) {
                $taskName = $taskLine.Substring(0, $taskSeparator)
                if ($taskName -ne 'PSModulePath') {
                    [Environment]::SetEnvironmentVariable($taskName, $taskLine.Substring($taskSeparator + 1), 'Process')
                }
            }
        }
        if (-not $taskMatchesPython) {
            & cmake -S $taskRepository -B $taskBuildPath '-U_Python_*' -DFALCOR_USE_SYSTEM_PYTHON=ON "-DPython_EXECUTABLE:FILEPATH=$taskPython" "-DPython_ROOT_DIR:PATH=$($taskPythonInfo.prefix)"
            if ($LASTEXITCODE -ne 0) { throw '无法将 Falcor 构建配置为 gs_env 的 Python。' }
        }
        & cmake --build $taskBuildPath --config Release --target Mogwai MyPathTracer ImageCompare falcor-copy-files -j 4
        if ($LASTEXITCODE -ne 0) { throw 'Falcor 构建失败。' }
    }
    $taskRuntime = Join-Path $taskBuildPath 'bin/Release'
    if ($BuildOnly) {
        Write-Host "编译完成（Conda gs_env / Python $($taskPythonInfo.version)）。"
        exit 0
    }
    if (-not (Test-Path -LiteralPath (Join-Path $taskRuntime 'plugins/MyPathTracer.dll'))) { throw 'MyPathTracer 插件尚未构建。' }
    Push-Location -LiteralPath $taskRuntime
    try { . ./setpath.ps1 } finally { Pop-Location }
    & $taskConda run --no-capture-output -n gs_env python -c 'import numpy,matplotlib'
    if ($LASTEXITCODE -ne 0) { throw 'gs_env 缺少依赖，请运行 conda run -n gs_env python -m pip install -r scripts/python/my_path_tracer/requirements.txt。' }
    & $taskConda run --no-capture-output -n gs_env python @taskArguments
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
