################################################################################
# MRS Version: 2.2.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../User/CRC16/CRC16.c 

C_DEPS += \
./User/CRC16/CRC16.d 

OBJS += \
./User/CRC16/CRC16.o 


EXPANDS += \
./User/CRC16/CRC16.c.234r.expand 



# Each subdirectory must supply rules for building sources it contributes
User/CRC16/%.o: ../User/CRC16/%.c
	@	riscv-none-embed-gcc -march=rv32ecxw -mabi=ilp32e -msmall-data-limit=0 -msave-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -Wunused -Wuninitialized -g -I"c:/Users/Bogdan/Desktop/=== VICTORIA ===/===SOFTWARE===/Dwch/Debug" -I"c:/Users/Bogdan/Desktop/=== VICTORIA ===/===SOFTWARE===/Dwch/Core" -I"c:/Users/Bogdan/Desktop/=== VICTORIA ===/===SOFTWARE===/Dwch/User" -I"c:/Users/Bogdan/Desktop/=== VICTORIA ===/===SOFTWARE===/Dwch/Peripheral/inc" -I"c:/Users/Bogdan/Desktop/=== VICTORIA ===/===SOFTWARE===/Dwch/User/CRC16" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

