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
install_inputs := $(call link_inputs,install.rsp)
objects := $(sort $(filter build/obj/%.o,$(main_inputs) $(install_inputs)))
object_dirs := $(sort $(patsubst %/,%,$(dir $(objects))))
programs := winwm.exe winwm_install.exe
staged := $(addprefix build/,$(programs))

.PHONY: all
all: $(addprefix bin/,$(programs))

build bin build/obj $(object_dirs):
	@if not exist "$(subst /,\,$@)" mkdir "$(subst /,\,$@)"
build/obj: | build
$(object_dirs): | build/obj

.SECONDEXPANSION:
build/obj/%.o: src/%.c compile.rsp Makefile | $$(@D)
	"$(CC)" @compile.rsp -MMD -MP -c $< -o $@
build/obj/lua/%.o: third_party/lua/%.c compile.rsp Makefile | $$(@D)
	"$(CC)" @compile.rsp -MMD -MP -c $< -o $@

build/install_res.o: src/install/install.rc src/install/app.manifest third_party/lua/LICENSE Makefile | build
	"$(WINDRES)" -I src/install -i $< -O coff -o $@

build/config_res.o: assets/config.rc assets/app.manifest assets/winwm.lua third_party/lua/LICENSE Makefile | build
	"$(WINDRES)" -I assets -i $< -O coff -o $@

$(staged): compile.rsp Makefile | build
build/winwm.exe: build.rsp lua.rsp $(filter %.o,$(main_inputs))
	"$(CC)" @compile.rsp $(main_inputs) -o $@
build/winwm_install.exe: install.rsp lua.rsp $(filter %.o,$(install_inputs))
	"$(CC)" @compile.rsp $(install_inputs) -o $@

# Publish in order after all links succeed. A running manager can lock the first file.
$(addprefix bin/,$(programs)): bin/%.exe: build/%.exe | bin $(staged)
	@copy /b /y "$(subst /,\,$<)" "$(subst /,\,$@).tmp" >nul && move /y "$(subst /,\,$@).tmp" "$(subst /,\,$@)" >nul
bin/winwm_install.exe: | bin/winwm.exe

-include $(objects:.o=.d)
