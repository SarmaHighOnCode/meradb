@echo off
rem Opens the MeraDB shell on the repo data folder (<repo>\data), inside the palantir database.
set "MERADB_DATA=%~dp0..\..\data"
"%~dp0..\..\cpp\build\meradb_cli.exe" shell --local -d palantir
