set(src_dirs
    EG800)

set(include_dirs
    EG800)

set(requires
    driver
    nvs_flash)

idf_component_register(SRC_DIRS ${src_dirs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${requires})
