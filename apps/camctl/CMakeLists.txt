add_executable(camctl
	main.cpp
)

target_link_libraries(camctl PRIVATE
	rkcam_core
)
