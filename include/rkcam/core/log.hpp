/**
*@file log.hpp
*@brief 
*@author HKH368
*@date 2026-05-29
*@return 
*/

#pragma once

#include <cstdio>

#define RKCAM_LOGI(fmt, ...) \
	std::printf("[INFO] " fmt "\n", ##__VA_ARGS__)

#define RKCAM_LOGE(fmt, ...) \
	std::print(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)


