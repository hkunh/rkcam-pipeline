add_executable(rkcamd
	main.cpp
)

target_link_libraries(rkcamd PRIVATE
	rkcam_core
	rkcam_pipeline_lib
)
