#define thisModuleName "input:android"
#include <base/android/sdk.hh>
#include <input/common/common.h>
#include <base/android/private.hh>
#include <util/jni.hh>
#include <android/input.h>
#include <android/configuration.h>
#include <dlfcn.h>

#include "common.hh"

namespace Input
{

static float (*AMotionEvent_getAxisValue)(const AInputEvent* motion_event, int32_t axis, size_t pointer_index) = 0;
static bool handleVolumeKeys = 0;
static bool allowKeyRepeats = 1;
static const int AINPUT_SOURCE_JOYSTICK = 0x01000010;
static const uint maxJoystickAxisPairs = 4; // 2 sticks + POV hat + L/R Triggers
static const uint maxSysInputDevs = MAX_DEVS;
static uint sysInputDevs = 0;
struct SysInputDevice
{
	constexpr SysInputDevice() { }
	int osId = 0;
	Device *dev = nullptr;
	char name[48] {0};
	bool axisBtnState[maxJoystickAxisPairs][4] { { 0 } };

	bool operator ==(SysInputDevice const& rhs) const
	{
		return osId == rhs.osId && string_equal(name, rhs.name);
	}
};
static SysInputDevice sysInputDev[maxSysInputDevs];
static Device *builtinKeyboardDev = nullptr;
static Device *virtualDev = nullptr;

// JNI classes/methods

class JObject
{
protected:
	jobject o = nullptr;
	constexpr JObject() { };
	constexpr JObject(jobject o): o(o) { };

public:
	operator jobject() const
	{
		return o;
	}
};

class AInputDeviceJ : public JObject
{
public:
	constexpr AInputDeviceJ(jobject inputDevice): JObject(inputDevice) { };

	static AInputDeviceJ getDevice(JNIEnv *j, jint id)
	{
		return AInputDeviceJ {getDevice_(j, id)};
	}

	static jintArray getDeviceIds(JNIEnv *j)
	{
		return (jintArray)getDeviceIds_(j);
	}

	jstring getName(JNIEnv *j)
	{
		return (jstring)getName_(j, o);
	}

	jint getSources(JNIEnv *j)
	{
		return getSources_(j, o);
	}

	jint getKeyboardType(JNIEnv *j)
	{
		return getKeyboardType_(j, o);
	}

	static jclass cls;
	static JavaClassMethod<jobject> getDeviceIds_, getDevice_;
	static JavaInstMethod<jobject> getName_, getKeyCharacterMap_;
	static JavaInstMethod<jint> getSources_, getKeyboardType_;
	static constexpr jint SOURCE_CLASS_BUTTON = 0x00000001, SOURCE_CLASS_POINTER = 0x00000002, SOURCE_CLASS_TRACKBALL = 0x00000004,
			SOURCE_CLASS_POSITION = 0x00000008, SOURCE_CLASS_JOYSTICK = 0x00000010;
	static constexpr jint SOURCE_KEYBOARD = 0x00000101, SOURCE_DPAD = 0x00000201, SOURCE_GAMEPAD = 0x00000401,
			SOURCE_TOUCHSCREEN = 0x00001002, SOURCE_MOUSE = 0x00002002, SOURCE_STYLUS = 0x00004002,
			SOURCE_TRACKBALL = 0x00010004, SOURCE_TOUCHPAD = 0x00100008, SOURCE_JOYSTICK = 0x01000010;

	static constexpr jint KEYBOARD_TYPE_NONE = 0,  KEYBOARD_TYPE_NON_ALPHABETIC = 1, KEYBOARD_TYPE_ALPHABETIC = 2;

	static void jniInit()
	{
		using namespace Base;
		cls = (jclass)eEnv()->NewGlobalRef(eEnv()->FindClass("android/view/InputDevice"));
		getDeviceIds_.setup(eEnv(), cls, "getDeviceIds", "()[I");
		getDevice_.setup(eEnv(), cls, "getDevice", "(I)Landroid/view/InputDevice;");
		getName_.setup(eEnv(), cls, "getName", "()Ljava/lang/String;");
		getSources_.setup(eEnv(), cls, "getSources", "()I");
		getKeyboardType_.setup(eEnv(), cls, "getKeyboardType", "()I");
	}
};

jclass AInputDeviceJ::cls = nullptr;
JavaClassMethod<jobject> AInputDeviceJ::getDeviceIds_, AInputDeviceJ::getDevice_;
JavaInstMethod<jobject> AInputDeviceJ::getName_, AInputDeviceJ::getKeyCharacterMap_;
JavaInstMethod<jint> AInputDeviceJ::getSources_, AInputDeviceJ::getKeyboardType_;

void setKeyRepeat(bool on)
{
	// always accept repeats on Android 3.1+ because 2+ devices pushing
	// the same button is considered a repeat by the OS
	if(Base::androidSDK() < 12)
	{
		logMsg("set key repeat %s", on ? "On" : "Off");
		allowKeyRepeats = on;
	}
}

void setHandleVolumeKeys(bool on)
{
	logMsg("set volume key use %s", on ? "On" : "Off");
	handleVolumeKeys = on;
}

bool sendInputToIME = 1;
void setEventsUseOSInputMethod(bool on)
{
	logMsg("set IME use %s", on ? "On" : "Off");
	sendInputToIME = on;
}

bool eventsUseOSInputMethod()
{
	return sendInputToIME;
}

static const char* aInputSourceToStr(uint source)
{
	switch(source)
	{
		case AINPUT_SOURCE_UNKNOWN: return "Unknown";
		case AINPUT_SOURCE_KEYBOARD: return "Keyboard";
		case AINPUT_SOURCE_DPAD: return "DPad";
		case AINPUT_SOURCE_TOUCHSCREEN: return "Touchscreen";
		case AINPUT_SOURCE_MOUSE: return "Mouse";
		case AINPUT_SOURCE_TRACKBALL: return "Trackball";
		case AINPUT_SOURCE_TOUCHPAD: return "Touchpad";
		case AINPUT_SOURCE_JOYSTICK: return "Joystick";
		case AINPUT_SOURCE_ANY: return "Any";
		default:  return "Unhandled value";
	}
}

static void handleKeycodesForSpecialDevices(const Input::Device &dev, int32_t &keyCode, int32_t &metaState)
{
	switch(dev.subtype)
	{
		#ifdef __ARM_ARCH_7A__
		bcase Device::SUBTYPE_XPERIA_PLAY:
		{
			if(unlikely(keyCode == (int)Keycode::ESCAPE && (metaState & AMETA_ALT_ON)))
			{
				keyCode = Keycode::GAME_B;
			}
		}
		#endif
		bdefault: break;
	}
}

static const Device *deviceForInputId(int osId)
{
	if(Base::androidSDK() < 12)
	{
		assert(devList.first()->map() == Event::MAP_KEYBOARD);
		return devList.first(); // head of list is always catch-all-android-input device
	}
	iterateTimes(sysInputDevs, i)
	{
		if(sysInputDev[i].osId == osId)
		{
			return sysInputDev[i].dev;
		}
	}
	return nullptr;
}

int32_t onInputEvent(AInputEvent* event)
{
	auto type = AInputEvent_getType(event);
	auto source = AInputEvent_getSource(event);
	switch(type)
	{
		case AINPUT_EVENT_TYPE_MOTION:
		{
			int eventAction = AMotionEvent_getAction(event);
			//logMsg("get motion event action %d", eventAction);

			switch(source)
			{
				case AINPUT_SOURCE_TRACKBALL:
				{
					//logMsg("from trackball");
					handleTrackballEvent(eventAction, AMotionEvent_getX(event, 0), AMotionEvent_getY(event, 0));
					return 1;
				}
				case AINPUT_SOURCE_TOUCHPAD: // TODO
				{
					//logMsg("from touchpad");
					return 0;
				}
				case AINPUT_SOURCE_TOUCHSCREEN:
				case AINPUT_SOURCE_MOUSE:
				{
					//logMsg("from touchscreen or mouse");
					uint action = eventAction & AMOTION_EVENT_ACTION_MASK;
					if(action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL)
					{
						// touch gesture ended
						handleTouchEvent(AMOTION_EVENT_ACTION_UP,
								AMotionEvent_getX(event, 0) - Base::window().rect.x,
								AMotionEvent_getY(event, 0) - Base::window().rect.y,
								AMotionEvent_getPointerId(event, 0));
						return 1;
					}
					uint actionPIdx = eventAction >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
					int pointers = AMotionEvent_getPointerCount(event);
					iterateTimes(pointers, i)
					{
						int pAction = action;
						// a pointer not performing the action just needs its position updated
						if(actionPIdx != i)
						{
							//logMsg("non-action pointer idx %d", i);
							pAction = AMOTION_EVENT_ACTION_MOVE;
						}
						handleTouchEvent(pAction,
							AMotionEvent_getX(event, i) - Base::window().rect.x,
							AMotionEvent_getY(event, i) - Base::window().rect.y,
							AMotionEvent_getPointerId(event, i));
					}
					return 1;
				}
				case AINPUT_SOURCE_JOYSTICK: // Joystick
				{
					auto dev = deviceForInputId(AInputEvent_getDeviceId(event));
					if(unlikely(!dev))
					{
						logWarn("discarding joystick input from unknown device ID: %d", AInputEvent_getDeviceId(event));
						return 0;
					}
					auto eventDevID = dev->devId;
					//logMsg("Joystick input from %s,%d", dev->name(), dev->devId);

					static const uint altBtnEvent[maxJoystickAxisPairs][4] =
					{
							{ Input::Keycode::LEFT, Input::Keycode::RIGHT, Input::Keycode::DOWN, Input::Keycode::UP },
							{ Keycode::JS2_XAXIS_NEG, Keycode::JS2_XAXIS_POS, Keycode::JS2_YAXIS_POS, Keycode::JS2_YAXIS_NEG },
							{ Keycode::JS3_XAXIS_NEG, Keycode::JS3_XAXIS_POS, Keycode::JS3_YAXIS_POS, Keycode::JS3_YAXIS_NEG },
							{ 0, Keycode::JS_LTRIGGER_AXIS, Keycode::JS_RTRIGGER_AXIS, 0 }
					};
					static const uint btnEvent[maxJoystickAxisPairs][4] =
					{
							{ Keycode::JS1_XAXIS_NEG, Keycode::JS1_XAXIS_POS, Keycode::JS1_YAXIS_POS, Keycode::JS1_YAXIS_NEG },
							{ Keycode::JS2_XAXIS_NEG, Keycode::JS2_XAXIS_POS, Keycode::JS2_YAXIS_POS, Keycode::JS2_YAXIS_NEG },
							{ Keycode::JS3_XAXIS_NEG, Keycode::JS3_XAXIS_POS, Keycode::JS3_YAXIS_POS, Keycode::JS3_YAXIS_NEG },
							{ 0, Keycode::JS_LTRIGGER_AXIS, Keycode::JS_RTRIGGER_AXIS, 0 }
					};

					auto &axisToBtnMap = dev->mapJoystickAxis1ToDpad ? altBtnEvent : btnEvent;
					auto &sysDev = sysInputDev[dev->idx];

//					if(AMotionEvent_getAxisValue)
//					{
//					logMsg("axis [%f %f] [%f %f] [%f %f] [%f %f]",
//							(double)AMotionEvent_getAxisValue(event, 0, 0), (double)AMotionEvent_getAxisValue(event, 1, 0),
//							(double)AMotionEvent_getAxisValue(event, 11, 0), (double)AMotionEvent_getAxisValue(event, 14, 0),
//							(double)AMotionEvent_getAxisValue(event, 15, 0), (double)AMotionEvent_getAxisValue(event, 16, 0),
//							(double)AMotionEvent_getAxisValue(event, 17, 0), (double)AMotionEvent_getAxisValue(event, 18, 0));
//					}

					iterateTimes(AMotionEvent_getAxisValue ? maxJoystickAxisPairs : 1, i)
					{
						static const int32_t AXIS_X = 0, AXIS_Y = 1, AXIS_Z = 11, AXIS_RZ = 14, AXIS_HAT_X = 15, AXIS_HAT_Y = 16,
								AXIS_LTRIGGER = 17, AXIS_RTRIGGER = 18;
						float pos[2];
						if(AMotionEvent_getAxisValue)
						{
							switch(i)
							{
								bcase 0:
									pos[0] = AMotionEvent_getAxisValue(event, AXIS_X, 0);
									pos[1] = AMotionEvent_getAxisValue(event, AXIS_Y, 0);
								bcase 1:
									pos[0] = AMotionEvent_getAxisValue(event, AXIS_Z, 0);
									pos[1] = AMotionEvent_getAxisValue(event, AXIS_RZ, 0);
								bcase 2:
									pos[0] = AMotionEvent_getAxisValue(event, AXIS_HAT_X, 0);
									pos[1] = AMotionEvent_getAxisValue(event, AXIS_HAT_Y, 0);
								bcase 3:
									pos[0] = AMotionEvent_getAxisValue(event, AXIS_LTRIGGER, 0);
									pos[1] = AMotionEvent_getAxisValue(event, AXIS_RTRIGGER, 0);
								bdefault: bug_branch("%d", i);
							}
						}
						else
						{
							pos[0] = AMotionEvent_getX(event, 0);
							pos[1] = AMotionEvent_getY(event, 0);
						}
						//logMsg("from Joystick, %f, %f", (double)pos[0], (double)pos[1]);
						forEachInArray(sysDev.axisBtnState[i], e)
						{
							if(i == 3 && (e_i == 0 || e_i == 3))
								continue; // skip negative test for trigger axis
							bool newState;
							switch(e_i)
							{
								case 0: newState = pos[0] < -0.5; break;
								case 1: newState = pos[0] > 0.5; break;
								case 2: newState = pos[1] > 0.5; break;
								case 3: newState = pos[1] < -0.5; break;
								default: bug_branch("%d", (int)e_i); break;
							}
							if(*e != newState)
							{
								onInputEvent(Event(eventDevID, Event::MAP_KEYBOARD, axisToBtnMap[i][e_i],
										newState ? PUSHED : RELEASED, 0, dev));
							}
							*e = newState;
						}
					}
					return 1;
				}
				default:
				{
					logWarn("from other source: %s, %dx%d", aInputSourceToStr(source), (int)AMotionEvent_getX(event, 0), (int)AMotionEvent_getY(event, 0));
					return 0;
				}
			}
		}
		bcase AINPUT_EVENT_TYPE_KEY:
		{
			auto keyCode = AKeyEvent_getKeyCode(event);
			//logMsg("key event, code: %d id: %d source: 0x%X repeat: %d action: %d", keyCode, AInputEvent_getDeviceId(event), source, AKeyEvent_getRepeatCount(event), AKeyEvent_getAction(event));
			if(unlikely(!keyCode)) // ignore "unknown" key codes
				return 0;
			if(!handleVolumeKeys &&
				(keyCode == (int)Keycode::VOL_UP || keyCode == (int)Keycode::VOL_DOWN))
			{
				return 0;
			}
			//auto isGamepad = bit_isMaskSet(source, AInputDeviceJ::SOURCE_GAMEPAD);

			if(allowKeyRepeats || AKeyEvent_getRepeatCount(event) == 0)
			{
				auto dev = deviceForInputId(AInputEvent_getDeviceId(event));
				if(unlikely(!dev))
				{
					assert(virtualDev);
					//logWarn("re-mapping unknown device ID %d to Virtual", AInputEvent_getDeviceId(event));
					dev = virtualDev;
				}
				auto metaState = AKeyEvent_getMetaState(event);
				handleKeycodesForSpecialDevices(*dev, keyCode, metaState);
				handleKeyEvent(keyCode, AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP ? 0 : 1, dev->devId, metaState & AMETA_SHIFT_ON, *dev);
			}
			return 1;
		}
	}
	logWarn("unhandled input event type %d", type);
	return 0;
}

static void JNICALL textInputEnded(JNIEnv* env, jobject thiz, jstring jStr)
{
	auto delegate = vKeyboardTextDelegate;
	vKeyboardTextDelegate.clear();
	if(delegate.hasCallback())
	{
		if(jStr)
		{
			const char *str = env->GetStringUTFChars(jStr, 0);
			logMsg("running text entry callback with text: %s", str);
			delegate.invoke(str);
			env->ReleaseStringUTFChars(jStr, str);
		}
		else
		{
			logMsg("canceled text entry callback");
			delegate.invoke(nullptr);
		}
	}
	else
	{
		logMsg("text entry has no callback");
	}
	Base::postDrawWindowIfNeeded();
}

bool dlLoadAndroidFuncs(void *libandroid)
{
	if(Base::androidSDK() < 12)
	{
		return 0;
	}
	// Google seems to have forgotten to put AMotionEvent_getAxisValue() in the NDK libandroid.so even though it's
	// present in at least Android 4.0, so we'll load it dynamically to be safe
	if((AMotionEvent_getAxisValue = (float (*)(const AInputEvent*, int32_t, size_t))dlsym(libandroid, "AMotionEvent_getAxisValue"))
			== 0)
	{
		logWarn("AMotionEvent_getAxisValue not found");
		return 0;
	}
	return 1;
}

void rescanDevices(bool firstRun)
{
	forEachInDLList(&Input::devList, e)
	{
		if(e.map() == Event::MAP_KEYBOARD || e.map() == Event::MAP_ICADE)
			e_it.removeElem();
	}
	indexDevices();
	virtualDev = nullptr;
	builtinKeyboardDev = nullptr;

	sysInputDevs = 0;
	using namespace Base;
	auto jID = AInputDeviceJ::getDeviceIds(eEnv());
	auto id = eEnv()->GetIntArrayElements(jID, 0);
	bool foundVirtual = 0;
	logMsg("checking input devices");
	iterateTimes(eEnv()->GetArrayLength(jID), i)
	{
		auto dev = AInputDeviceJ::getDevice(eEnv(), id[i]);
		jint src = dev.getSources(eEnv());
		jstring jName = dev.getName(eEnv());
		if(!jName)
		{
			logWarn("no name from device %d, id %d", i, id[i]);
			continue;
		}
		const char *name = eEnv()->GetStringUTFChars(jName, 0);
		bool hasKeys = src & AInputDeviceJ::SOURCE_CLASS_BUTTON;
		logMsg("#%d: %s, id %d, source %X", i, name, id[i], src);
		if(hasKeys && !devList.isFull() && sysInputDevs != MAX_DEVS)
		{
			auto &sysInput = sysInputDev[sysInputDevs];
			uint devId = 0;
			// find the next available ID number for devices with this name, starting from 0
			forEachInDLList(&devList, e)
			{
				if(e.map() != Event::MAP_KEYBOARD)
					continue;
				if(string_equal(e.name(), name) && e.devId == devId)
					devId++;
			}
			sysInput.osId = id[i];
			string_copy(sysInput.name, name);
			Input::addDevice((Device){devId, Event::MAP_KEYBOARD, Device::TYPE_BIT_KEY_MISC, sysInput.name});
			auto newDev = devList.last();
			sysInput.dev = newDev;
			mem_zero(sysInput.axisBtnState);
			if(id[i] == 0) // built-in keyboard is always id 0 according to Android docs
			{
				builtinKeyboardDev = newDev;
			}
			else if(id[i] == -1)
			{
				foundVirtual = 1;
				virtualDev = newDev;
				newDev->setTypeBits(newDev->typeBits() | Device::TYPE_BIT_VIRTUAL);
			}

			if(bit_isMaskSet(src, AInputDeviceJ::SOURCE_GAMEPAD)
					&& !bit_isMaskSet(src, AInputDeviceJ::SOURCE_TOUCHSCREEN)) // ignore some odd devices like "MHLRCP"
			{
				bool isGamepad = 1;
				#ifdef __ARM_ARCH_7A__
				if(strstr(name, "-zeus"))
				{
					logMsg("detected Xperia Play gamepad");
					newDev->subtype = Device::SUBTYPE_XPERIA_PLAY;
				}
				else if(string_equal(name, "sii9234_rcp"))
				{
					// sii9234_rcp on Samsung devices like Galaxy S2, may claim to be a gamepad & full keyboard
					// but has only special function keys
					logMsg("ignoring extra device bits");
					src = 0;
					isGamepad = 0;
				}
				else
				#endif
				if(string_equal(name, "Sony PLAYSTATION(R)3 Controller"))
				{
					logMsg("detected PS3 gamepad");
					newDev->subtype = Device::SUBTYPE_PS3_CONTROLLER;
				}
				else
				{
					logMsg("detected a gamepad");
				}
				if(isGamepad)
					newDev->setTypeBits(Device::TYPE_BIT_GAMEPAD);
			}
			if(bit_isMaskSet(src, AInputDeviceJ::SOURCE_KEYBOARD)
					&& dev.getKeyboardType(eEnv()) == AInputDeviceJ::KEYBOARD_TYPE_ALPHABETIC)
			{
				newDev->setTypeBits(newDev->typeBits() | Device::TYPE_BIT_KEYBOARD);
				logMsg("detected an alpha-numeric keyboard");
			}
			if(bit_isMaskSet(src, AInputDeviceJ::SOURCE_JOYSTICK))
			{
				newDev->setTypeBits(newDev->typeBits() | Device::TYPE_BIT_JOYSTICK);
				logMsg("detected a joystick");
			}

			logMsg("added to list with device id %d", newDev->devId);
			sysInputDevs++;
		}
		eEnv()->ReleaseStringUTFChars(jName, name);
		eEnv()->DeleteLocalRef(dev);
	}
	eEnv()->ReleaseIntArrayElements(jID, id, 0);

	if(!foundVirtual)
	{
		if(sysInputDevs == MAX_DEVS || devList.isFull())
		{
			// remove last device to make room
			devList.remove(*sysInputDev[sysInputDevs-1].dev);
			sysInputDevs--;
		}
		logMsg("no \"Virtual\" device id found, adding one");
		Input::addDevice((Device){0, Event::MAP_KEYBOARD, Device::TYPE_BIT_VIRTUAL | Device::TYPE_BIT_KEYBOARD | Device::TYPE_BIT_KEY_MISC, "Virtual"});
		auto newDev = devList.last();
		auto &sysInput = sysInputDev[sysInputDevs];
		sysInput.osId = -1;
		sysInput.dev = newDev;
		string_copy(sysInput.name, "Virtual");
		virtualDev = newDev;
		sysInputDevs++;
	}

	if(!firstRun)
	{
		// TODO: dummy event, apps will just re-scan whole device list for now
		onInputDevChange((DeviceChange){ 0, Event::MAP_KEYBOARD, DeviceChange::ADDED });
	}
}

CallResult init()
{
	if(Base::androidSDK() >= 12)
	{
		AInputDeviceJ::jniInit();
		rescanDevices(1);
	}
	else
	{
		// no multi-input device support
		Device genericKeyDev { 0, Event::MAP_KEYBOARD,
			Device::TYPE_BIT_VIRTUAL | Device::TYPE_BIT_KEYBOARD | Device::TYPE_BIT_KEY_MISC, "Key Input (All Devices)" };
		#ifdef __ARM_ARCH_7A__
		if(Base::runningDeviceType() == Base::DEV_TYPE_XPERIA_PLAY)
		{
			genericKeyDev.subtype = Device::SUBTYPE_XPERIA_PLAY;
		}
		#endif
		Input::addDevice(genericKeyDev);
		builtinKeyboardDev = devList.last();
	}
	return OK;
}

}
