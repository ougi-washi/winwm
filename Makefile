SHELL := cmd.exe
.SHELLFLAGS := /d /c
.DELETE_ON_ERROR:

ifeq ($(origin CC),default)
CC := gcc
endif
WINDRES ?= windres

# Keep the direct GCC response files as the source of target inputs and link order.
lua_sources := $(file <lua.rsp)
link_inputs = $(patsubst third_party/lua/%.c,build/obj/lua/%.o,$(patsubst src/%.c,build/obj/%.o,$(filter-out @compile.rsp,$(subst @lua.rsp,$(lua_sources),$(file <$(1))))))
main_inputs := $(call link_inputs,build.rsp)
objects := $(sort $(filter build/obj/%.o,$(main_inputs)))
object_dirs := $(sort $(patsubst %/,%,$(dir $(objects))))
.PHONY: all
all: bin/winwm.exe
	@for %%f in (bin\winwm_install.exe bin\winwm_recovery.exe build\winwm_install.exe build\winwm_recovery.exe) do @if exist "%%f" (del /q "%%f" & if exist "%%f" exit /b 1)

build bin build/obj $(object_dirs):
	@if not exist "$(subst /,\,$@)" mkdir "$(subst /,\,$@)"
build/obj: | build
$(object_dirs): | build/obj

.SECONDEXPANSION:
build/obj/%.o: src/%.c compile.rsp Makefile | $$(@D)
	"$(CC)" @compile.rsp -MMD -MP -c $< -o $@
build/obj/lua/%.o: third_party/lua/%.c compile.rsp Makefile | $$(@D)
	"$(CC)" @compile.rsp -MMD -MP -c $< -o $@

build/config_res.o: assets/config.rc assets/app.manifest assets/winwm.lua third_party/lua/LICENSE Makefile | build
	"$(WINDRES)" -I assets -i $< -O coff -o $@

build/winwm.exe: build.rsp compile.rsp Makefile lua.rsp $(filter %.o,$(main_inputs)) | build
	"$(CC)" @compile.rsp $(main_inputs) -o $@

bin/winwm.exe: build/winwm.exe | bin
	@copy /b /y "$(subst /,\,$<)" "$(subst /,\,$@).tmp" >nul
	@move /y "$(subst /,\,$@).tmp" "$(subst /,\,$@)" >nul 2>nul || powershell -NoProfile -NonInteractive -Command "[IO.File]::Replace('$@.tmp','$@','build/winwm.previous')"

-include $(objects:.o=.d)
