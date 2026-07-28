@echo off
REM ============================================================
REM ABProtect 最强保护构建脚本
REM 步骤: 编译 → 加壳（全保护 + 云计算 + 水印）
REM ============================================================

setlocal

REM ===== 配置区域（请修改为你的实际值）=====
set "SOLUTION_DIR=D:\ab3\ab3\ab3"
set "SOLUTION_FILE=%SOLUTION_DIR%\新伪心跳.sln"
set "OUTPUT_DIR=%SOLUTION_DIR%\x64\Release"
set "EXE_NAME=新伪心跳.exe"
set "PROTECTED_NAME=新伪心跳_protected.exe"

set "ABPROTECT_CLI=D:\mp\output\client\ABProtectCLI.exe"

REM ===== ABProtect 服务器配置（替换为你的正式地址）=====
set "CLOUD_SERVER=YOUR_SERVER_IP:9999"
set "CLOUD_KEY=00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF"
set "VERIFY_URL=https://YOUR_SERVER_IP/api/verify"

REM ===== 水印（每次发布可更改，用于追踪泄露）=====
set "WATERMARK=AB3-v4.0-Build%DATE:~0,10%-%USERNAME%"

echo ============================================================
echo  ABProtect 最强保护构建
echo ============================================================
echo.

REM ===== Step 1: 编译 Release x64 =====
echo [1/3] 正在编译 Release x64...
msbuild "%SOLUTION_FILE%" /p:Configuration=Release /p:Platform=x64 /m /v:minimal
if %ERRORLEVEL% NEQ 0 (
    echo [错误] 编译失败！
    exit /b 1
)
echo [1/3] 编译成功
echo.

REM ===== Step 2: ABProtect 加壳 =====
echo [2/3] 正在加壳（全部保护 + 云计算 + 水印）...
echo       云服务器: %CLOUD_SERVER%
echo       水印: %WATERMARK%
echo.

"%ABPROTECT_CLI%" "%OUTPUT_DIR%\%EXE_NAME%" "%OUTPUT_DIR%\%PROTECTED_NAME%" ^
    --vm ^
    --cff ^
    --subleq ^
    --mutate 3 ^
    --anti-vm ^
    --hardened ^
    --cloud %CLOUD_SERVER% ^
    --cloud-key %CLOUD_KEY% ^
    --cloud-data "%OUTPUT_DIR%\新伪心跳.abcloud" ^
    --online-verify "%VERIFY_URL%" ^
    --watermark "%WATERMARK%"

if %ERRORLEVEL% NEQ 0 (
    echo [错误] 加壳失败！
    exit /b 2
)
echo [2/3] 加壳成功
echo.

REM ===== Step 3: 输出结果 =====
echo [3/3] 构建完成！
echo ============================================================
echo  输出文件:
echo    加壳程序: %OUTPUT_DIR%\%PROTECTED_NAME%
echo    云数据:   %OUTPUT_DIR%\新伪心跳.abcloud
echo.
echo  部署步骤:
echo    1. 将 新伪心跳.abcloud 上传到 ABProtect Server
echo    2. 在服务端管理面板生成卡密
echo    3. 分发 %PROTECTED_NAME% 给用户
echo ============================================================

endlocal
