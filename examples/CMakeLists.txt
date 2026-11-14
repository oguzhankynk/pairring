add_executable(pairring_shm_pingpong shm_pingpong.cpp)
# rt: shm_open (a stub on glibc >= 2.34, needed on older ones).
target_link_libraries(pairring_shm_pingpong PRIVATE pairring::pairring rt)
pairring_set_warnings(pairring_shm_pingpong)
pairring_enable_sanitizers(pairring_shm_pingpong)
