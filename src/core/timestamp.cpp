/**
*@file src/core/timestamp.cpp
*@brief 
*@author HKH368
*@date 2026-05-29
*@return 
*/
#include <chrono>
#include <cstdint>
namespace rkcam{

int64_t now_us()
{
	auto now = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::microseconds>(
			now.time_since_epoch()
			).count();
}
}
