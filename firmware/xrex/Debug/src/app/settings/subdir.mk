################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/app/settings/device_identity.c \
../src/app/settings/profile_store.c \
../src/app/settings/settings.c \
../src/app/settings/settings_codec.c

OBJS += \
./src/app/settings/device_identity.o \
./src/app/settings/profile_store.o \
./src/app/settings/settings.o \
./src/app/settings/settings_codec.o

C_DEPS += \
./src/app/settings/device_identity.d \
./src/app/settings/profile_store.d \
./src/app/settings/settings.d \
./src/app/settings/settings_codec.d


# Each subdirectory must supply rules for building sources it contributes
src/app/settings/%.o src/app/settings/%.su src/app/settings/%.cyclo: ../src/app/settings/%.c src/app/settings/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32G474xx -c -I../Core/Inc -I../src/app -I../src/app/commands -I../src/app/control -I../src/app/protection -I../src/app/settings -I../src/app/sim -I../src/bsp/stm32g4 -I../src/config -I../src/drivers -I../src/middleware/scpi -I../build/generated -I../Drivers/STM32G4xx_HAL_Driver/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32G4xx/Include -I../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-src-2f-app-2f-settings

clean-src-2f-app-2f-settings:
	-$(RM) ./src/app/settings/device_identity.cyclo ./src/app/settings/device_identity.d ./src/app/settings/device_identity.o ./src/app/settings/device_identity.su ./src/app/settings/profile_store.cyclo ./src/app/settings/profile_store.d ./src/app/settings/profile_store.o ./src/app/settings/profile_store.su ./src/app/settings/settings.cyclo ./src/app/settings/settings.d ./src/app/settings/settings.o ./src/app/settings/settings.su ./src/app/settings/settings_codec.cyclo ./src/app/settings/settings_codec.d ./src/app/settings/settings_codec.o ./src/app/settings/settings_codec.su

.PHONY: clean-src-2f-app-2f-settings

