################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/app/control/closed_loop.c \
../src/app/control/control_runtime.c \
../src/app/control/feedback.c \
../src/app/control/hvps_curve.c \
../src/app/control/pfm.c \
../src/app/control/pid_core.c \
../src/app/control/shot_profile.c \
../src/app/control/state_machine.c \
../src/app/control/supply_mode.c \
../src/app/control/target_profile.c

OBJS += \
./src/app/control/closed_loop.o \
./src/app/control/control_runtime.o \
./src/app/control/feedback.o \
./src/app/control/hvps_curve.o \
./src/app/control/pfm.o \
./src/app/control/pid_core.o \
./src/app/control/shot_profile.o \
./src/app/control/state_machine.o \
./src/app/control/supply_mode.o \
./src/app/control/target_profile.o

C_DEPS += \
./src/app/control/closed_loop.d \
./src/app/control/control_runtime.d \
./src/app/control/feedback.d \
./src/app/control/hvps_curve.d \
./src/app/control/pfm.d \
./src/app/control/pid_core.d \
./src/app/control/shot_profile.d \
./src/app/control/state_machine.d \
./src/app/control/supply_mode.d \
./src/app/control/target_profile.d


# Each subdirectory must supply rules for building sources it contributes
src/app/control/%.o src/app/control/%.su src/app/control/%.cyclo: ../src/app/control/%.c src/app/control/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32G474xx -c -I../Core/Inc -I../src/app -I../src/app/commands -I../src/app/control -I../src/app/protection -I../src/app/settings -I../src/app/sim -I../src/bsp/stm32g4 -I../src/config -I../src/drivers -I../src/middleware/scpi -I../build/generated -I../Drivers/STM32G4xx_HAL_Driver/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32G4xx/Include -I../Drivers/CMSIS/Include -O2 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-src-2f-app-2f-control

clean-src-2f-app-2f-control:
	-$(RM) ./src/app/control/closed_loop.cyclo ./src/app/control/closed_loop.d ./src/app/control/closed_loop.o ./src/app/control/closed_loop.su ./src/app/control/control_runtime.cyclo ./src/app/control/control_runtime.d ./src/app/control/control_runtime.o ./src/app/control/control_runtime.su ./src/app/control/feedback.cyclo ./src/app/control/feedback.d ./src/app/control/feedback.o ./src/app/control/feedback.su ./src/app/control/hvps_curve.cyclo ./src/app/control/hvps_curve.d ./src/app/control/hvps_curve.o ./src/app/control/hvps_curve.su ./src/app/control/pfm.cyclo ./src/app/control/pfm.d ./src/app/control/pfm.o ./src/app/control/pfm.su ./src/app/control/pid_core.cyclo ./src/app/control/pid_core.d ./src/app/control/pid_core.o ./src/app/control/pid_core.su ./src/app/control/shot_profile.cyclo ./src/app/control/shot_profile.d ./src/app/control/shot_profile.o ./src/app/control/shot_profile.su ./src/app/control/state_machine.cyclo ./src/app/control/state_machine.d ./src/app/control/state_machine.o ./src/app/control/state_machine.su ./src/app/control/supply_mode.cyclo ./src/app/control/supply_mode.d ./src/app/control/supply_mode.o ./src/app/control/supply_mode.su ./src/app/control/target_profile.cyclo ./src/app/control/target_profile.d ./src/app/control/target_profile.o ./src/app/control/target_profile.su

.PHONY: clean-src-2f-app-2f-control

