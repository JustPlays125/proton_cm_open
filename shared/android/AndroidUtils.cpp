#include "AndroidUtils.h"
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include <jni.h>
#include <android/log.h>
#include <cstdlib>
#include <cstdarg>
#include <cassert>
#include "PlatformSetup.h"
#include "BaseApp.h"
#include "App.h"

#include "FileSystem/FileSystemZip.h"
#include "Irrlicht/IrrlichtManager.h"

#include "cocos2d.h"
using namespace cocos2d;

#define C_DELAY_BEFORE_RESTORING_SURFACES_MS 1

JavaVM* g_pJavaVM = NULL;

//const char* GetAppName();
//const char* GetBundlePrefix();
//const char* GetBundleName();

uint32		g_callAppResumeASAPTimer = 0;
bool		g_pauseASAP = false;
int			g_musicPos = 0;
std::string	g_musicToPlay;

static bool	s_preferSDCardForUserStorage	= false;
static bool s_bFirstTime		= true;
static char s_ClassName[128]	= {0};

//used to be a temporary holding place so android can access it because I'm too lazy to figure
//out how to make/pass java structs
static OSMessage						s_lastOSMessage; 
static std::list<AndroidMessageCache*>	s_messageCache;
static pthread_mutex_t					s_mouselock;

int g_winVideoScreenX = 0;
int g_winVideoScreenY = 0;


void StringReplace(const std::string& what, const std::string& with, std::string& in);
std::vector<std::string> StringTokenize(const std::string& theString, const std::string& theDelimiter);


extern "C" 
{
	JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) 
	{
		JNIEnv* env;
		if (vm->GetEnv((void**)&env, JNI_VERSION_1_4) != JNI_OK) 
		{
			LogMsg("JNI_OnLoad(): GetEnv failed");
			return -1;
		}
		g_pJavaVM = vm; //save to use this for the rest of the app.  Not neccesarily safe to use the Env passed in though.

		LogMsg("JNI_OnLoad(): you are here");

		return JNI_VERSION_1_4;
	}
}

JNIEnv* GetJavaEnv()
{
	JNIEnv* env = NULL;

	assert(g_pJavaVM);

	g_pJavaVM->GetEnv((void **)&env, JNI_VERSION_1_4);
	if (!env)
	{
		LogError("Env is null, something is terrible wrong");
		return NULL;
	}
	
	return env;
}

int GetPrimaryGLX() 
{
	return g_winVideoScreenX;
}

int GetPrimaryGLY()
{
	return g_winVideoScreenY;
}	

void LogMsg( const char* traceStr, ... )
{
	std::string	strapp;
	va_list		argsVA;
	
	int		logSize = 4096;
	char	buffer[logSize];
	
	memset( (void*)buffer, 0, logSize );

	va_start( argsVA, traceStr );
	vsnprintf( buffer, logSize, traceStr, argsVA );
	va_end( argsVA );
			
	//__android_log_write(ANDROID_LOG_INFO,GetAppName(), buffer);
	strapp = WstringToString(GetAppName());
	__android_log_write(ANDROID_LOG_INFO, strapp.c_str(), buffer);

	if (IsBaseAppInitted())
	{
		BaseApp::GetBaseApp()->GetConsole()->AddLine(buffer);
	}
}


std::string GetBaseAppPath()
{
	return ""; //we mount the assets as zip, there really isn't a base path
}

char* GetAndroidMainClassName()
{
	std::string package;

	if (s_bFirstTime)
	{
		s_bFirstTime = false;
		package = std::string(GetBundlePrefix())+std::string(GetBundleName())+"/Main";
		StringReplace(".", "/", package);
		
		//sprintf(name, package.c_str()); //fixed bug to build
		strcpy(s_ClassName, package.c_str());
	}

	return s_ClassName;
}

std::string GetSavePathBasic()
{
	JNIEnv* env = GetJavaEnv();
	
	if (!env) 
		return "";

	jclass		cls = env->FindClass(GetAndroidMainClassName());
	jmethodID	mid = env->GetStaticMethodID(cls, "get_docdir", "()Ljava/lang/String;");
	jstring		ret = (jstring)env->CallStaticObjectMethod(cls, mid);
	const char* ss	= env->GetStringUTFChars(ret,0);
	std::string tmp = ss;
	env->ReleaseStringUTFChars(ret, ss);

	return std::string(tmp)+"/";
}

/*void SetPreferSDCardForStorage(bool bNew)
{
	s_preferSDCardForUserStorage = bNew;
}*/

std::string GetSavePath()
{
	std::string retString;

	LogMsg("Starting get save path..");

	if (s_preferSDCardForUserStorage)
	{
		std::string storageDir = GetAppCachePath();
		
		if (!storageDir.empty()) 
			return storageDir;
	}
	LogMsg("continuing get save path..");

	retString = GetSavePathBasic();
	
#ifdef _DEBUG
	LogMsg("Save dir is %s", std::string(retString).c_str());
#endif

	return retString;
}

std::string GetAPKFile()
{
	JNIEnv *env = GetJavaEnv();
	if (!env)
	{
		LogMsg("GetAPKFile>  Error, can't do this yet, no java environment");
		return "";
	}

	LogMsg("Getting apk file for %s from the Java side...",GetAndroidMainClassName());
	jclass cls = env->FindClass(GetAndroidMainClassName());
	jmethodID mid = env->GetStaticMethodID(cls,	"get_apkFileName", "()Ljava/lang/String;");
	jstring ret;
	ret = (jstring)env->CallStaticObjectMethod(cls, mid);
	const char* ss	= env->GetStringUTFChars(ret,0);
	std::string tmp = ss;
	env->ReleaseStringUTFChars(ret, ss);
	return std::string(tmp);
}

std::string GetAppCachePath()
{
	LogMsg("Getting app cache..");

	JNIEnv *env = GetJavaEnv();
	if (!env) 
		return "";

	//first see if we can access an external storage method
	jclass		cls = env->FindClass(GetAndroidMainClassName());
	jmethodID	mid = env->GetStaticMethodID(cls, "get_externaldir", "()Ljava/lang/String;");
	jstring		ret = (jstring)env->CallStaticObjectMethod(cls, mid);
	const char* ss	= env->GetStringUTFChars(ret,0);
	std::string tmp = ss;
	env->ReleaseStringUTFChars(ret, ss);
	std::string retString = std::string(tmp);

	if (!retString.empty())
	{
		retString += std::string("/Android/data/")+GetBundlePrefix()+GetBundleName()+"/files/";
		
		return retString;
	}

	retString = GetSavePathBasic();

	return retString; //invalid
}

void LaunchEmail(std::string subject, std::string content)
{

}

void FireAchievement(std::string achievement)
{
	JNIEnv *env = GetJavaEnv();
	LogMsg("Attempting to fire Achievement %s", achievement.c_str());

	if (!env) 
		return;
	jclass cls = env->FindClass(GetAndroidMainClassName());
	jmethodID mid = env->GetStaticMethodID(cls,	"HandleAchievement", "(Ljava/lang/String;)V");
	env->CallStaticVoidMethod(cls, mid, env->NewStringUTF(achievement.c_str()));
}

void LaunchURL(std::string url)
{
	JNIEnv *env = GetJavaEnv();
	LogMsg("Launching %s", url.c_str());

	if (!env) 
		return;
	jclass cls = env->FindClass(GetAndroidMainClassName());
	jmethodID mid = env->GetStaticMethodID(cls,	"LaunchURL", "(Ljava/lang/String;)V");
	env->CallStaticVoidMethod(cls, mid, env->NewStringUTF(url.c_str()));
}

std::string GetClipboardText()
{
	JNIEnv *env = GetJavaEnv();
	if (!env) 
		return "";

	jclass cls = env->FindClass(GetAndroidMainClassName());
	jmethodID mid = env->GetStaticMethodID(cls,	"get_clipboard", "()Ljava/lang/String;");
	jstring ret;
	ret = (jstring)env->CallStaticObjectMethod(cls, mid);
	const char* ss	= env->GetStringUTFChars(ret,0);
	std::string tmp = ss;
	env->ReleaseStringUTFChars(ret, ss);
	return std::string(tmp);
}

bool IsIPhone3GS()
{
	return false;
}

bool IsDesktop()
{
	if (GetEmulatedPlatformID() == PLATFORM_ID_ANDROID) return false;
	return true;
}

ePlatformID GetPlatformID()
{
	return PLATFORM_ID_ANDROID;
}

bool IsIphoneOriPad()
{
	return false;
}

bool IsIphone()
{
	return false;
}

bool IsIphone4()
{
	return false; 
}

std::string GetDeviceID()
{
	JNIEnv *env = GetJavaEnv();
	if (!env) return "";

	jclass cls = env->FindClass(GetAndroidMainClassName());
	jmethodID mid = env->GetStaticMethodID(cls,	"get_deviceID",	"()Ljava/lang/String;");
	jstring ret;
	ret = (jstring)env->CallStaticObjectMethod(cls, mid);
	const char* ss	= env->GetStringUTFChars(ret,0);
	std::string tmp = ss;
	env->ReleaseStringUTFChars(ret, ss);
	return std::string(tmp);
}

std::string GetMacAddress()
{
	//todo
	return "";
}

bool IsIPAD()
{
	return false;
}

float GetDeviceOSVersion()
{
	//TODO
	return 0.0f;
}

eDeviceMemoryClass GetDeviceMemoryClass()
{
	return C_DEVICE_MEMORY_CLASS_2;
}

bool CheckDay(const int year, const int month, const int day)
{
	struct timeval  nowSecs;
    gettimeofday(&nowSecs, NULL);
	time_t  nowtime = nowSecs.tv_sec;
	struct tm *now = localtime(&nowtime);
	if (now->tm_year < 1900)
	{
		now->tm_year += 1900;
	}
	now->tm_mon++;
	LogMsg("Comparing against date year %d, month %d, day %d", now->tm_year, now->tm_mon, now->tm_mday);
	if ((now->tm_mday == day) && (now->tm_mon == month) && (now->tm_year == year))
	{
		return true;
	}
	return false;
}

bool LaterThanNow(const int year, const int month, const int day)
{
	struct timeval  nowSecs;
    gettimeofday(&nowSecs, NULL);
	time_t  nowtime = nowSecs.tv_sec;
	struct tm *now = localtime(&nowtime);
	if (now->tm_year < 1900)
	{
		now->tm_year += 1900;
	}
	now->tm_mon++;
	LogMsg("Comparing against date year %d, month %d, day %d", now->tm_year, now->tm_mon, now->tm_mday);

	if (now->tm_year< year )
	{
		return false;
	}
	if (now->tm_year> year )
	{
		return true;
	}
	// year must be equal
	if (now->tm_mon < month )
	{
		return false;
	}
	if (now->tm_mon > month )
	{
		return true;
	}
	// month must be equal
	if (now->tm_mday < day )
	{
		return false;
	}
	if (now->tm_mday > day )
	{
		return true;
	}
	return false;
}

unsigned int GetSystemTimeTick()
{

	/*
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_usec/1000 + tv.tv_sec*1000;
	*/

	//More resistent to playing with the date to change timing in games

	static unsigned int incrementingTimer = 0;
	static double buildUp = 0;
	static double lastTime = 0;

	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	double timeDouble = time.tv_sec*1000 + time.tv_nsec/1000000;

	double change = timeDouble -lastTime;
	if (change > 0 && change < 500)
	{
		incrementingTimer += change;
	}
	lastTime = timeDouble;

	return incrementingTimer;

}

double GetSystemTimeAccurate()
{
	return double(GetSystemTimeTick());
}

unsigned int GetFreeMemory()
{
	return 0; //don't care on the PC
}

std::string g_string;

void SetLastStringInput( std::string s )
{
	g_string = s;
}

std::string GetLastStringInput()
{
	return g_string;
}

eNetworkType IsNetReachable( std::string url )
{
	return C_NETWORK_WIFI;
}

int GetSystemData()
{
	return C_PIRATED_NO;
}

void RemoveFile( std::string fileName, bool bAddSavePath)
{
	
	if (bAddSavePath)
	{
		fileName = GetSavePath()+fileName;
	}

	if (unlink(fileName.c_str()) == -1)
	{
		switch (errno)
		{
		case EACCES: 
			LogMsg("Warning: Unable to delete file %s, no access", fileName.c_str());
			break;
		case EBUSY: 
			LogError("Warning: Unable to delete file %s, file is being used", fileName.c_str());
			break;
		case EPERM: 
			LogMsg("Warning: Unable to delete file %s, may be a dir", fileName.c_str());
			break;
		case EROFS: 
			LogMsg("Warning: Unable to delete file %s, File system is read only", fileName.c_str());
			break;
		default:
			//LogMsg("Warning: Unable to delete file %s, unknown error", fileName.c_str());
			//file doesn't exist
			break;
		}
	}
}

std::string GetRegionString()
{
	JNIEnv *env = GetJavaEnv();
	if (!env) return "";

	jclass cls = env->FindClass(GetAndroidMainClassName());
	jmethodID mid = env->GetStaticMethodID(cls,	"get_region", "()Ljava/lang/String;");
	jstring ret;
	ret = (jstring)env->CallStaticObjectMethod(cls, mid);
	const char* ss	= env->GetStringUTFChars(ret,0);
	std::string tmp = ss;
	env->ReleaseStringUTFChars(ret, ss);
	return std::string(tmp);
}

bool IsAppInstalled(std::string packageName)
{
	JNIEnv *env = GetJavaEnv();
	
	if (!env) 
		return false;
	
	jclass cls = env->FindClass(GetAndroidMainClassName());
	jmethodID mid = env->GetStaticMethodID(cls,	"is_app_installed",	"(Ljava/lang/String;)I");
	return env->CallStaticIntMethod(cls, mid, env->NewStringUTF(packageName.c_str())) != 0;
}

//month is 1-12 btw
int GetDaysSinceDate(int month,int day, int year)
{
	//LogMsg("GetDaysSinceDate url not done");
	assert(!"no!");
	return 0;
}

void CreateDirectoryRecursively(std::string basePath, std::string path)
{
#ifdef _DEBUG
	//LogMsg("CreateDirectoryRecursively: %s, path is %s", basePath.c_str(), path.c_str());
#endif
	
	JNIEnv *env = GetJavaEnv();
	if (!env) 
		return;

	jclass cls = env->FindClass(GetAndroidMainClassName());
	jmethodID mid = env->GetStaticMethodID(cls,	"create_dir_recursively", "(Ljava/lang/String;Ljava/lang/String;)V");
	jstring ret;
	env->CallStaticVoidMethod(cls, mid, env->NewStringUTF(basePath.c_str()), env->NewStringUTF(path.c_str()));
	return;
}

bool RTCreateDirectory(const std::string &dir_name)
{
#ifdef _DEBUG
	LogMsg("CreateDirectory: %s", dir_name.c_str());
#endif

	std::string temp = dir_name;
	CreateDirectoryRecursively("", temp);
	return true;
}

std::vector<std::string> GetDirectoriesAtPath(std::string path)
{
	std::vector<std::string> v;

	dirent * buf, * ent;
	DIR *dp;

	dp = opendir(path.c_str());
	if (!dp)
	{
		LogError("GetDirectoriesAtPath: opendir failed");
		return v;
	}

	buf = (dirent*) malloc(sizeof(dirent)+512);
	while (readdir_r(dp, buf, &ent) == 0 && ent)
	{
		if (ent->d_name[0] == '.' && ent->d_name[1] == 0) continue;
		if (ent->d_name[0] == '.' && ent->d_name[1] == '.' && ent->d_name[2] == 0) continue;

		//LogMsg("Got %s. type %d", ent->d_name, int(ent->d_type));
		
		if (ent->d_type == DT_DIR)
		{
			v.push_back(ent->d_name);
		}
	}

	free (buf);
	closedir(dp);
	return v;
}

std::vector<std::string> GetFilesAtPath(std::string path)
{
#ifdef _DEBUG
	//LogMsg("GetFilesAtPath: %s", path.c_str());
#endif

	std::vector<std::string> v;
	dirent * buf, * ent;
	DIR *dp;

	dp = opendir(path.c_str());
	if (!dp)
	{
		LogError("GetDirectoriesAtPath: opendir failed");
		return v;
	}

	buf = (dirent*) malloc(sizeof(dirent)+512);
	while (readdir_r(dp, buf, &ent) == 0 && ent)
	{
		if (ent->d_name[0] == '.' && ent->d_name[1] == 0) continue;
		if (ent->d_name[0] == '.' && ent->d_name[1] == '.' && ent->d_name[2] == 0) continue;

		//LogMsg("Got %s. type %d", ent->d_name, int(ent->d_type));
		if (ent->d_type == DT_REG) //regular file
		{
			v.push_back(ent->d_name);
		}
	}

	free (buf);
	closedir(dp);
	return v;
}

bool RemoveDirectoryRecursively(std::string path)
{
	//LogMsg(" RemoveDirectoryRecursively: %s", path.c_str());
	
	dirent * buf, * ent;
	DIR *dp;

	dp = opendir(path.c_str());
	if (!dp)
	{
		LogError("RemoveDirectoryRecursively: opendir failed");
		return false;
	}

	buf = (dirent*) malloc(sizeof(dirent)+512);
	while (readdir_r(dp, buf, &ent) == 0 && ent)
	{
		
		if (ent->d_name[0] == '.' && ent->d_name[1] == 0) continue;
		if (ent->d_name[0] == '.' && ent->d_name[1] == '.' && ent->d_name[2] == 0) continue;

		//LogMsg("Got %s. type %d", ent->d_name, int(ent->d_type));
		if (ent->d_type == DT_REG) //regular file
		{
			std::string fName = path+std::string("/")+ent->d_name;
			//LogMsg("Deleting %s", fName.c_str());
			unlink( fName.c_str());
		}

		if (ent->d_type == DT_DIR) //regular file
		{
			std::string fName = path+std::string("/")+ent->d_name;
			//LogMsg("Entering DIR %s",fName.c_str());
			if (!RemoveDirectoryRecursively(fName.c_str()))
			{
				LogError("Error removing dir %s", fName.c_str());
				break;
			}
		}
	}

	free (buf);
	closedir(dp);

	//delete the final dir as well
	rmdir( path.c_str());
	return true; //success
}

bool CheckIfOtherAudioIsPlaying()
{
	return false;
}

/*void CreateAppCacheDirIfNeeded()
{
}*/

void NotifyOSOfOrientationPreference(eOrientationMode orientation)
{
}

bool HasVibration()
{
	return true;
}

void MouseKeyProcess(int method, AndroidMessageCache* amsg, unsigned int* qsize)
{
	pthread_mutex_lock(&s_mouselock);

	AndroidMessageCache* store_msg;
	
	switch(method)
	{
		case 0:
			s_messageCache.push_back(amsg);
			break;

		case 1:
			store_msg		= s_messageCache.front();
			
			amsg->type		= store_msg->type;
			amsg->x			= store_msg->x;
			amsg->y			= store_msg->y;
			amsg->finger	= store_msg->finger;
			
			s_messageCache.pop_front();
			delete store_msg;
			
			break;

		case 2:
			*qsize = s_messageCache.size();
			break;
	}
		
	pthread_mutex_unlock(&s_mouselock);
}

void CheckTouchCommand()
{
#ifdef _IRR_COMPILE_WITH_GUI_	
	irr::SEvent	ev;
#endif			
	
	int					keyid;
	unsigned int		qsize;
	AndroidMessageCache	amessage;

	MouseKeyProcess(2, NULL, &qsize);
		
	if( qsize >= 1 )
	{
		MouseKeyProcess(1, &amessage, NULL);
		
		switch (amessage.type)
		{
			case ACTION_DOWN:
				//by stone
#ifdef _IRR_COMPILE_WITH_GUI_
				ev.MouseInput.Event 		= irr::EMIE_LMOUSE_PRESSED_DOWN;
				ev.EventType            	= irr::EET_MOUSE_INPUT_EVENT;
				ev.MouseInput.ButtonStates 	= 0;
				ev.MouseInput.X				= amessage.x;
				ev.MouseInput.Y				= amessage.y;
				IrrlichtManager::GetIrrlichtManager()->GetDevice()->postEventFromUser(ev);
#endif
				keyid = 0;
				g_pApp->HandleTouchesBegin(1, &keyid, &amessage.x, &amessage.y);

				break;

			case ACTION_UP:
				//by stone
#ifdef _IRR_COMPILE_WITH_GUI_
				ev.MouseInput.Event 		= irr::EMIE_LMOUSE_LEFT_UP;
				ev.EventType            	= irr::EET_MOUSE_INPUT_EVENT;
				ev.MouseInput.ButtonStates 	= 0;
				ev.MouseInput.X				= amessage.x;
				ev.MouseInput.Y				= amessage.y;
				IrrlichtManager::GetIrrlichtManager()->GetDevice()->postEventFromUser(ev);
#endif
				keyid = 0;
				g_pApp->HandleTouchesEnd(1, &keyid, &amessage.x, &amessage.y);
				break;

			case ACTION_MOVE:
				keyid = 0;
				g_pApp->HandleTouchesMove(1, &keyid, &amessage.x, &amessage.y);
				break;

			default:
				break;
		}
	}
}

void AppResize( JNIEnv*  env, jobject  thiz, jint w, jint h )
{
	std::string		apkpath;
	FileSystemZip*	pFileSystem = NULL;

	g_winVideoScreenX = w;
	g_winVideoScreenY = h;
		
	if (!BaseApp::GetBaseApp()->IsInitted())
	{
		srand( (unsigned)time(NULL) );

#ifdef _DEBUG
	LogMsg("Setup screen to %d %d", w, h);
#endif
		SetupScreenInfo(GetPrimaryGLX(), GetPrimaryGLY(), ORIENTATION_PORTRAIT);
				
		apkpath 	= GetAPKFile();
		
#ifdef _DEBUG
		LogMsg("Initializing BaseApp.  APK filename is %s", apkpath.c_str());
#endif				
		pFileSystem = new FileSystemZip();

		if( pFileSystem->Init_unz(apkpath) )
		{
			LogMsg("APK based Filesystem mounted.");
		}
		else
		{
			LogMsg("Error finding APK file to load resources (%s", apkpath.c_str());
		}

		pFileSystem->SetRootDirectory("assets");

		FileManager::GetFileManager()->MountFileSystem(pFileSystem);
				
		if (!BaseApp::GetBaseApp()->Init())
		{
			LogMsg("Unable to initalize BaseApp");
		}
		
		CCShaderCache::sharedShaderCache();

		CCSize size = CCSize(w, h);
		CCDirector::sharedDirector()->setWinSize(size);
		CCDirector::sharedDirector()->setContentScaleFactor(1.0f);
		CCDirector::sharedDirector()->setOpenGLView(NULL);

		pthread_mutex_init(&s_mouselock, NULL);

		//let's also create our save directory on the sd card if needed
		//so we don't get errors when just assuming we can save
		//settings later in the app.
		CreateDirectoryRecursively("", GetAppCachePath());
	}
}

void AppUpdate(JNIEnv*  env)
{
	if (g_pauseASAP)
	{
		LogMsg("Pause");
		
		g_pauseASAP = false;	
		
		//unLoad surface to IrrlichtManager::OnUnloadSurfaces()
		BaseApp::GetBaseApp()->m_sig_unloadSurfaces();
		//s_bSurfacesUnloaded = true;

		//signal to IrrlichtManager::OnLoadSurfaces()
		BaseApp::GetBaseApp()->m_sig_loadSurfaces();
		
		//GetAudioManager()->Kill(); //already done in AppPause
	}
	else
	{
		if (g_callAppResumeASAPTimer != 0 && g_callAppResumeASAPTimer < GetSystemTimeTick())
		{
			g_callAppResumeASAPTimer = 0;
			
			GetAudioManager()->Init(); //nothing do inside

			//replay music after AppPause
			if (!g_musicToPlay.empty())
			{
				GetAudioManager()->Play(g_musicToPlay, GetAudioManager()->GetLastMusicLooping(), true, false, true);
				GetAudioManager()->SetPos(GetAudioManager()->GetLastMusicID(), g_musicPos);
			}
		}
	}
}

void AppRender(JNIEnv*  env)
{
	if (BaseApp::GetBaseApp()->IsInBackground() || g_pauseASAP) 
		return;
	
	BaseApp::GetBaseApp()->CheckInitAgain();
	BaseApp::GetBaseApp()->ClearGLBuffer();
	
	BaseApp::GetBaseApp()->Update();
	BaseApp::GetBaseApp()->Draw();
	
	CCDirector::sharedDirector()->setGLDefaultValues();
	CCDirector::sharedDirector()->mainLoop();
	CCDirector::sharedDirector()->RestoreGLValues();

	CheckTouchCommand();
}

void AppInit(JNIEnv* env)
{
	LogMsg("AppInit finish");
}

void AppDone(JNIEnv*  env)
{
	CCDirector::sharedDirector()->end();
	CCDirector::sharedDirector()->purgeDirector();
	
	pthread_mutex_destroy(&s_mouselock);

	LogMsg("Killing base app.");
}

void AppPause(JNIEnv*  env)
{
	//Note: Thread update isn't going to happen until they COME back, which means we aren't killing
	//all used textures.  But this fixes problems with the engine trying to reload textures as it
	//notices they are killed because the pause is run concurrently with the game update/render
	//thread going.

	if (g_pauseASAP)
	{
		LogMsg("Got android AppPause, ignoring as we've already triggered it");
		return;
	}

#ifdef _DEBUG
	LogMsg("Got android AppPause");
#endif

	g_pauseASAP = true;

	if (GetAudioManager()->IsPlaying(GetAudioManager()->GetLastMusicID()))
	{
		g_musicToPlay	= GetAudioManager()->GetLastMusicFileName();
		g_musicPos 		= GetAudioManager()->GetPos(GetAudioManager()->GetLastMusicID());
	} 
	else
	{
		g_musicToPlay.clear();
		g_musicPos = 0;

	}

	GetAudioManager()->Kill(); //StopMusic() of AudioManagerAndroid::Kill()
}

void AppResume(JNIEnv*  env)
{
	g_callAppResumeASAPTimer = GetSystemTimeTick() + C_DELAY_BEFORE_RESTORING_SURFACES_MS;

#ifdef _DEBUG
	LogMsg("Queuing resume: %u (now is %u)", g_callAppResumeASAPTimer, GetTick(TIMER_SYSTEM));
#endif
}

void AppOnTouch( JNIEnv*  env, jobject jobj,jint action, jfloat x, jfloat y, jint finger)
{
	unsigned int			qsize;
	AndroidMessageCache*	amessage;

	switch (action)
	{
		case ACTION_DOWN:
			amessage			= new AndroidMessageCache();
			amessage->type		= ACTION_DOWN;
			amessage->x			= x;
			amessage->y			= y;
			amessage->finger	= finger;
			MouseKeyProcess(0, amessage, NULL);
			break;

		case ACTION_UP:
			amessage			= new AndroidMessageCache();
			amessage->type		= ACTION_UP;
			amessage->x			= x;
			amessage->y			= y;
			amessage->finger	= finger;
			MouseKeyProcess(0, amessage, NULL);
			break;

		case ACTION_MOVE:
			MouseKeyProcess(2, NULL, &qsize);

			if( qsize <= 0 )
			{
				amessage			= new AndroidMessageCache();
				amessage->type		= ACTION_MOVE;
				amessage->x			= x;
				amessage->y			= y;
				amessage->finger	= finger;
				MouseKeyProcess(0, amessage, NULL);
			}
			break;

		default:
			break;
	}
}

void AppOnSendGUIEx(JNIEnv*  env, jobject thiz,jint messageType, jint parm1, jint parm2, jint finger )
{
	MessageManager::GetMessageManager()->SendGUIEx((eMessageType)messageType, (float)parm1, (float)parm2, finger);  
}

void AppOnSendGUIStringEx(JNIEnv*  env, jobject thiz,jint messageType, jint parm1, jint parm2, jint finger, jstring s )
{
	const char * ss=env->GetStringUTFChars(s,0);
	std::string str = ss;
	env->ReleaseStringUTFChars(s, ss);

	MessageManager::GetMessageManager()->SendGUIStringEx((eMessageType)messageType, (float)parm1, (float)parm2, finger, str);  

}

void AppOnKey( JNIEnv*  env, jobject jobj, jint type, jint keycode, jint c)
{
	
#ifdef _DEBUG
	LogMsg("Native Got type %d, keycode %d, key %d (%c)", type, keycode, c, (char(c)));
#endif

	switch (keycode)
	{
		
		//case 4: e.v = KEY_BACK; break; // KEYCODE_BACK
		//case 82: e.v = KEY_MENU; break; // KEYCODE_MENU
		//case 84: e.v = KEY_SEARCH; break; // KEYCODE_SEARCH
	case 66: //enter
		c = 13;
		break;
	case 67: //back space
		c = 8;
		break;

	case 99:
		keycode = VIRTUAL_DPAD_BUTTON_LEFT;
		break;

	case 100:
		keycode = VIRTUAL_DPAD_BUTTON_UP;
		break;


	case 4:
		//actually this will never get hit, the java side will send VIRTUAL_DPAD_BUTTON_RIGHT directly, because it
		//shares stuff with the back button and has to detect which one it is on that side
		keycode = VIRTUAL_DPAD_BUTTON_RIGHT;
		break;
	case 109:
		keycode = VIRTUAL_DPAD_SELECT;
		break;
	case 108:
		keycode =  VIRTUAL_DPAD_START;
		break;

	case 102:
		keycode =  VIRTUAL_DPAD_LBUTTON;
	break;
	case 103:
		keycode =  VIRTUAL_DPAD_RBUTTON;
		break;

	}
	
	if (keycode >= VIRTUAL_KEY_BACK)
	{
		
		if (GetIsUsingNativeUI())
		{
			//hitting back with the keyboard open?  Just pretend they closed the keyboard.
			SetIsUsingNativeUI(false);
			return;
		} 
		else
		{
			c = keycode;
		}
		

		c = keycode;
	}

	switch (type)
	{
		case 1: //keydown
			MessageManager::GetMessageManager()->SendGUI(MESSAGE_TYPE_GUI_CHAR, (float)c, (float)1);  
			
			if (c < 128) c = toupper(c);
			MessageManager::GetMessageManager()->SendGUI(MESSAGE_TYPE_GUI_CHAR_RAW, (float)c, (float)1);  
			break;

		case 0: //key up
			if (c < 128) c = toupper(c);
			MessageManager::GetMessageManager()->SendGUI(MESSAGE_TYPE_GUI_CHAR_RAW, (float)c, 0.0f);  
			break;
	}
}


int AppOSMessageGet(JNIEnv* env)
{
	if (!IsBaseAppInitted())
	{
		return 0;
	}

	/*while (!s_messageCache.empty())
	{
		AndroidMessageCache *pM = &s_messageCache.front();
		
		ConvertCoordinatesIfRequired(pM->x, pM->y);

		MessageManager::GetMessageManager()->SendGUIEx(pM->type, pM->x, pM->y, pM->finger);
		s_messageCache.pop_front();
	}*/

	while (!BaseApp::GetBaseApp()->GetOSMessages()->empty())
	{
		//check for special messages that we don't want to pass on
		s_lastOSMessage = BaseApp::GetBaseApp()->GetOSMessages()->front();
		if (s_lastOSMessage.m_type == OSMessage::MESSAGE_CHECK_CONNECTION)
		{
				//pretend we did it
				MessageManager::GetMessageManager()->SendGUI(MESSAGE_TYPE_OS_CONNECTION_CHECKED, (float)RT_kCFStreamEventOpenCompleted, 0.0f);	
				BaseApp::GetBaseApp()->GetOSMessages()->pop_front();
				continue;
		}
		break;
	}

	if (!BaseApp::GetBaseApp()->GetOSMessages()->empty())
	{
		s_lastOSMessage = BaseApp::GetBaseApp()->GetOSMessages()->front();
		BaseApp::GetBaseApp()->GetOSMessages()->pop_front();
		
		return s_lastOSMessage.m_type;
	}

	return OSMessage::MESSAGE_NONE;
}

jstring AppGetLastOSMessageString(JNIEnv* env)
{
	 return(env->NewStringUTF(s_lastOSMessage.m_string.c_str()));
}

jstring AppGetLastOSMessageString2(JNIEnv* env)
{
	return(env->NewStringUTF(s_lastOSMessage.m_string2.c_str()));
}

jstring AppGetLastOSMessageString3(JNIEnv* env)
{
	return(env->NewStringUTF(s_lastOSMessage.m_string3.c_str()));
}

float AppGetLastOSMessageX(JNIEnv* env)
{
	return s_lastOSMessage.m_x;
}

float AppGetLastOSMessageY(JNIEnv* env)
{
	return s_lastOSMessage.m_y;
}

float AppGetLastOSMessageParm1(JNIEnv* env)
{
	return s_lastOSMessage.m_parm1;
}

// JAKE ADDED - MachineWorks needs this, so please leave.
void AppOnJoypadButtons(JNIEnv* env, jobject jobj, jint key, jint value)
{

#ifdef RT_MOGA_ENABLED
	//for Seth's GamepadManagerMoga implementation, so it can be abstracted like any other gamepad
	//LogMsg("Received key %d, value %d", key, value);
	VariantList vList((uint32)MESSAGE_TYPE_GUI_JOYPAD_BUTTONS, (uint32) key, (uint32)value);
	BaseApp::GetBaseApp()->m_sig_joypad_events(&vList);
#else
	//LogMsg("Jakes: Received key %d, value %d", key, value);
	MessageManager::GetMessageManager()->SendGUI(MESSAGE_TYPE_GUI_JOYPAD_BUTTONS, Variant(key, value, 0.0f));
#endif

}

void AppOnJoypad(JNIEnv* env, jobject jobj, jfloat xL, jfloat yL, jfloat xR, jfloat yR)
{
	LogMsg("Got %.2f, %.2f and %.2f, %.2f", xL, yL, xR, yR);

#ifdef RT_MOGA_ENABLED
	//for Seth's GamepadManagerMoga implementation, so it can be abstracted like any other gamepad
	VariantList vList((uint32)MESSAGE_TYPE_GUI_JOYPAD, xL, yL, xR, yR);
	BaseApp::GetBaseApp()->m_sig_joypad_events(&vList);
#else
	//legacy, for Jake's stuff
	VariantList vList( xL, yL, xR, yR);
	MessageManager::GetMessageManager()->SendGUI(MESSAGE_TYPE_GUI_JOYPAD, vList);
#endif
}

void AppOnJoypadConnection(JNIEnv* env, jobject jobj, jint connect)
{
//	LogMsg("Received connect value %d", connect);
#ifdef RT_MOGA_ENABLED
	//for Seth's GamepadManagerMoga implementation, so it can be abstracted like any other gamepad
	VariantList vList((uint32)MESSAGE_TYPE_GUI_JOYPAD_CONNECT, (uint32)connect);
	BaseApp::GetBaseApp()->m_sig_joypad_events(&vList);
#else
	{
		MessageManager::GetMessageManager()->SendGUI(MESSAGE_TYPE_GUI_JOYPAD_CONNECT, Variant(connect, 0.0f, 0.0f));
	}
#endif
}
// Jake End

void AppOnTrackball(JNIEnv* env, jobject jobj, jfloat x, jfloat y)
{
	//LogMsg("Got %.2f, %.2f", x, y);
	MessageManager::GetMessageManager()->SendGUI(MESSAGE_TYPE_GUI_TRACKBALL,  Variant(x, y, 0.0f));
}

void AppOnAccelerometerUpdate(JNIEnv* env, jobject jobj, jfloat x, jfloat y, jfloat z)
{
	//convert to about the same format as iPhone and webOS.  I used the nexus one to calibrate this.. are other phones different?

	x *= -(1.0f/8.3f);
	y *= -(1.0f/8.3f);
	z *= -(1.0f/8.3f);

	MessageManager::GetMessageManager()->SendGUI(MESSAGE_TYPE_GUI_ACCELEROMETER,  Variant(x, y, z));
}


void ForceVideoUpdate()
{

}

/*bool IsDirectoryDateNewerThan(std::string dir, int day, int month, int year)
{
	struct stat st;
	int ierr = stat (dir.c_str(), &st);
	if (ierr != 0) 
	{
		LogMsg("Unable to get date of %s", dir.c_str());
		return true;
	}
	
	time_t t = st.st_mtime;
	struct tm* my_tm = localtime(&t);
	//fix to match what was passed in
	my_tm->tm_year += 1900;
	my_tm->tm_mday += 1;
	my_tm->tm_mon += 1;

	//LogMsg("Date is %d, %d, %d", my_tm->tm_mday, my_tm->tm_mon, my_tm->tm_year);
	if (my_tm->tm_year > year) 
		return true;
	
	if (my_tm->tm_mon > month) 
		return true;
	
	if (my_tm->tm_mday > day) 
		return true;

	return true;
}*/
