SHELL := cmd.exe
.SHELLFLAGS := /d /c
.DELETE_ON_ERROR:

ifeq ($(origin CC),default)
CC := gcc
endif
WINDRES ?= windres

# Keep the direct GCC response files as the source of target inputs and link order.
link_inputs = $(patsubst src/%.c,build/obj/%.o,$(filter-out @compile.rsp,$(file <$(1))))
main_inputs := $(call link_inputs,build.rsp)
recovery_inputs := $(call link_inputs,recovery.rsp)
install_inputs := $(call link_inputs,install.rsp)
objects := $(sort $(filter build/obj/%.o,$(main_inputs) $(recovery_inputs) $(install_inputs)))
object_dirs := $(sort $(patsubst %/,%,$(dir $(objects))))
programs := winwm.exe winwm_recovery.exe winwm_install.exe
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

build/install_res.o: src/install/install.rc src/install/app.manifest Makefile | build
	"$(WINDRES)" -I src/install -i $< -O coff -o $@

$(staged): compile.rsp Makefile | build
build/winwm.exe: build.rsp $(filter %.o,$(main_inputs))
	"$(CC)" @compile.rsp $(main_inputs) -o $@
build/winwm_recovery.exe: recovery.rsp $(filter %.o,$(recovery_inputs))
	"$(CC)" @compile.rsp $(recovery_inputs) -o $@
build/winwm_install.exe: install.rsp $(filter %.o,$(install_inputs))
	"$(CC)" @compile.rsp $(install_inputs) -o $@

# Publish in order after all links succeed. A running manager can lock the first file.
$(addprefix bin/,$(programs)): bin/%.exe: build/%.exe | bin $(staged)
	@copy /b /y "$(subst /,\,$<)" "$(subst /,\,$@).tmp" >nul && move /y "$(subst /,\,$@).tmp" "$(subst /,\,$@)" >nul
bin/winwm_recovery.exe: | bin/winwm.exe
bin/winwm_install.exe: | bin/winwm_recovery.exe

-include $(objects:.o=.d)
