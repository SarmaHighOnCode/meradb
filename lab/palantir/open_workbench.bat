@echo off
rem Opens the MeraDB desktop workbench. It opens <repo>\data (all databases, palantir included) by itself.
start "" "%~dp0..\..\cpp\build\meradb_gui.exe"
