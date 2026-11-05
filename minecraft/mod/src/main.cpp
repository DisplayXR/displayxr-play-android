#include "Probe.hpp"

#include <pl/Mod.hpp>

#include <chrono>
#include <thread>

void crash_log_install();

class DisplayXR3DMod
{
public:
	static DisplayXR3DMod &instance()
	{
		static DisplayXR3DMod mod;
		return mod;
	}

	bool load(pl::mod::ModContext &ctx)
	{
		ctx.logger().info("DisplayXR 3D probe loading from {}", ctx.modRootPath().string());
		crash_log_install();
		// Debug: keep the crash logger in front of Breakpad, which installs later.
		std::thread([] {
			for (int i = 0; i < 600; ++i) {
				crash_log_install();
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		}).detach();
		dxr3d::probe_load(ctx.javaVm(), ctx.dataDir());
		return true;
	}
	bool enable(pl::mod::ModContext &) { dxr3d::probe_enable(); return true; }
	bool disable(pl::mod::ModContext &) { dxr3d::probe_disable(); return true; }
	bool unload(pl::mod::ModContext &) { dxr3d::probe_disable(); return true; }
};

PL_REGISTER_MOD(DisplayXR3DMod, DisplayXR3DMod::instance())
