# Without REQUIRES, main depends on every component, CardenzaHAL included (whatever its folder is called).
idf_component_register(SRCS "main.cpp" INCLUDE_DIRS ".")
