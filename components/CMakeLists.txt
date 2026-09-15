set(src_dirs
			MOTOR
            PWM
			AS608_ESP-IDF
			EXTI
			)

set(include_dirs
			MOTOR
            PWM
			AS608_ESP-IDF
			EXTI
			)

set(requires
            driver
			esp_driver_gpio
            esp_timer
			esp_driver_ledc
			)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)