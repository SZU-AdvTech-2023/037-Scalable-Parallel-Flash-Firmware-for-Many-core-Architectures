set DIR=%cd%

set TASK0_PROJ_NAME=core0
set TASK1_PROJ_NAME=core2
set TASK2_PROJ_NAME=core1
set TASK3_PROJ_NAME=core3

set WORKSPACE=%1

mklink /D "%WORKSPACE%\%TASK0_PROJ_NAME%\src\task" "%DIR%\TASK0"
mklink /D "%WORKSPACE%\%TASK1_PROJ_NAME%\src\task" "%DIR%\TASK1"
mklink /D "%WORKSPACE%\%TASK2_PROJ_NAME%\src\task" "%DIR%\TASK2"
mklink /D "%WORKSPACE%\%TASK3_PROJ_NAME%\src\task" "%DIR%\TASK3"

mklink /D "%WORKSPACE%\%TASK0_PROJ_NAME%\src\hostif" "%DIR%\hostif"
mklink /D "%WORKSPACE%\%TASK3_PROJ_NAME%\src\hostif" "%DIR%\hostif"

mklink /D "%WORKSPACE%\%TASK0_PROJ_NAME%\src\config" "%DIR%\config"
mklink /D "%WORKSPACE%\%TASK1_PROJ_NAME%\src\config" "%DIR%\config"
mklink /D "%WORKSPACE%\%TASK2_PROJ_NAME%\src\config" "%DIR%\config"
mklink /D "%WORKSPACE%\%TASK3_PROJ_NAME%\src\config" "%DIR%\config"

mklink /D "%WORKSPACE%\%TASK0_PROJ_NAME%\src\queue" "%DIR%\queue"
mklink /D "%WORKSPACE%\%TASK1_PROJ_NAME%\src\queue" "%DIR%\queue"
mklink /D "%WORKSPACE%\%TASK2_PROJ_NAME%\src\queue" "%DIR%\queue"
mklink /D "%WORKSPACE%\%TASK3_PROJ_NAME%\src\queue" "%DIR%\queue"