set(src_dirs
			LED
			Key
			Heat
			OLED)

set(include_dirs
			LED
			Key
			Heat
			OLED)

set(requires
			driver
			esp_adc)

idf_component_register(SRC_DIRS ${src_dirs}
					  INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)
