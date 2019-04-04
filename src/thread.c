#include <newxpl.h>
#include <stdio.h>
#include <stdlib.h>
#include <xpltypes.h>
#include <xplthread.h>

#ifdef GPROF
# include <sys/time.h>
#endif

#define DebugAssert( arg )

/*
typedef struct
{
	XplBool				running;
	char				*identity;
} XplThreadGroup, *XplThreadGroupID;

typedef struct XplThread
{
	XplBool				running;
	XplThreadGroup		*group;
	void				*context;
	int					exitCode;
	void				(*destroyCB)(struct XplThread *);
} XplThread, *XplThreadID;

	running:	thread state
	group:		group it belongs to
	context:	that was passed to XplThreadStart()
	exitCode:	return value of function passed to XplThreadStart()
	destroyCB:	optional callback when thread is destroyed

XplThread *XplThreadStart( XplThreadGroup *group, int (*userFunction)( XplThread * ), void *context );
	group:			(optional) thread group to use
	userFunction:	code to execute
	context:		(optional) data to associate with thread
	NULL on failure

void XplThreadFree( XplThread **thread );
	thread:		address of handle from XplThreadStart()

XplThreadFree() may be immediately called on a handle after starting a thread, this will not
cause the thread to die.
The thread handle will be useful in future API's, waiting on threads, signaling threads etc.

*/

#define MAX_SIGNAL_STACK	16
#define MAX_USER_DATA		16

#ifdef WIN32

typedef DWORD		_PlatformThreadID;

#define _GetPlatformThreadID()	GetCurrentThreadId()

EXPORT int _XplSemaInit( _XplSema *sema, int initialCount )
{
	if( sema )
	{
		// lpSemaphoreAttributes, lInitialCount, lMaximumCount, lpName
		*sema = CreateSemaphore( NULL, initialCount, LONG_MAX, NULL );
		return errno = 0;
	}
	return errno = EINVAL;
}

#define _HandleDestroy( h )		\
{								\
	if( h )						\
	{							\
		CloseHandle( *h );		\
		return errno = 0;		\
	}							\
	return errno = EINVAL;		\
}

#define _TimedWait( handle, milliseconds )	\
{											\
	if( handle )							\
	{										\
		switch( WaitForSingleObject( *handle, milliseconds ) )	\
		{									\
			case WAIT_OBJECT_0:				\
				return errno = 0;			\
											\
			case WAIT_ABANDONED:			\
			case WAIT_FAILED:				\
				break;						\
											\
			case WAIT_TIMEOUT:				\
				return errno = ETIMEDOUT;	\
		}									\
		return errno = EPERM;				\
	}										\
	return errno = EINVAL;					\
}

EXPORT int _XplSemaDestroy( _XplSema *sema )
{
	_HandleDestroy( sema );
}

EXPORT int _XplSemaWait( _XplSema *sema )
{
	_TimedWait( sema, INFINITE );
}

EXPORT int _XplSemaTimedWait( _XplSema *sema, int64 milliseconds )
{
	if( -1 == milliseconds )
	{
		return _XplSemaWait( sema );
	}
	_TimedWait( sema, milliseconds );
}

EXPORT int _XplSemaPost( _XplSema *sema )
{
	if( sema )
	{
		ReleaseSemaphore( *sema, 1, NULL );
		return errno = 0;
	}
	return errno = EINVAL;
}

typedef struct _SEMAPHORE_BASIC_INFORMATION {
  ULONG                   CurrentCount;
  ULONG                   MaximumCount;
} SEMAPHORE_BASIC_INFORMATION, *PSEMAPHORE_BASIC_INFORMATION;

typedef enum _SEMAPHORE_INFORMATION_CLASS {
    SemaphoreBasicInformation
} SEMAPHORE_INFORMATION_CLASS, *PSEMAPHORE_INFORMATION_CLASS;

typedef NTSTATUS (NTAPI *_NtQuerySemaphore)(
    HANDLE SemaphoreHandle,
    DWORD SemaphoreInformationClass, /* Would be SEMAPHORE_INFORMATION_CLASS */
    PVOID SemaphoreInformation,      /* but this is to much to dump here     */
    ULONG SemaphoreInformationLength,
    PULONG ReturnLength OPTIONAL
);

_NtQuerySemaphore NtQuerySemaphore = NULL;

EXPORT int _XplSemaValue( _XplSema *sema, int *value )
{
	SEMAPHORE_BASIC_INFORMATION	info;

	if( !NtQuerySemaphore )
	{
		NtQuerySemaphore = (_NtQuerySemaphore)GetProcAddress (GetModuleHandle ("ntdll.dll"), "NtQuerySemaphore");
		if( !NtQuerySemaphore )
		{
			return errno = ENOSYS;
		}
	}
	if( sema )
	{
		if( ERROR_SUCCESS == NtQuerySemaphore( *sema, 0, &info, sizeof( info ), NULL ) )
		{
			*value = info.CurrentCount;
			return errno = 0;
		}
		return errno = ENOSYS;
	}
	return errno = EINVAL;
}

EXPORT int _XplMutexInit( _XplMutex *mutex, XplBool recursive )
{
	if( mutex )
	{
		// plMutexAttributes, bInitialOwner, lpName
		*mutex = CreateMutex( NULL, FALSE, NULL );
		return errno = 0;
	}
	return errno = EINVAL;
}

EXPORT int _XplMutexDestroy( _XplMutex *mutex )
{
	_HandleDestroy( mutex );
}

EXPORT int _XplMutexLock( _XplMutex *mutex )
{
	_TimedWait( mutex, INFINITE );
}

EXPORT int _XplMutexUnlock( _XplMutex *mutex )
{
	if( mutex )
	{
		ReleaseMutex( *mutex );
		return errno = 0;
	}
	return errno = EINVAL;
}

#endif	// WIN32

#if defined(LINUX) || defined(S390RH) || defined(MACOSX)
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>
#undef __USE_GNU

typedef pthread_t			_PlatformThreadID;
#define _GetPlatformThreadID()	pthread_self()

EXPORT int _XplSemaInit( _XplSema *sema, int initialCount )
{
	if( sema )
	{
		if( !sem_init( sema, 0, initialCount ) )
		{
			errno = 0;
		}
		return errno;
	}
	return errno = EINVAL;
}

EXPORT int _XplSemaDestroy( _XplSema *sema )
{
	if( sema )
	{
		if( !sem_destroy( sema ) )
		{
			errno = 0;
		}
		return errno;
	}
	return errno = EINVAL;
}

EXPORT int _XplSemaWait( _XplSema *sema )
{
	int	retries;

	if( sema )
	{
		for(retries=0;retries<500;)
		{
			if( !sem_wait( sema ) )
			{
				return errno = 0;
			}
			if( EINTR == errno )
			{
				continue;
			}
			fprintf( stderr, "sem_wait() failure on thread %p, errno: %d, retries: %d\n", (void *)_GetPlatformThreadID(), errno, retries );
			XplDelay( retries * 5 );
			retries++;
		}
		DebugAssert( EINTR == errno );
		return errno;
	}
	return errno = EINVAL;
}

EXPORT int _XplSemaTimedWait( _XplSema *sema, int64 milliseconds )
{
	if( -1 == milliseconds )
	{
		return _XplSemaWait( sema );
	}
	if( sema )
	{
		struct timespec	ts;
		int64			now, endTime;

		if( !clock_gettime( CLOCK_REALTIME, &ts ) )
		{
			now = ( ts.tv_sec * 1000 );
			now += ( ts.tv_nsec / 1000 );
			endTime = now + milliseconds;

			for(;;)
			{
				ts.tv_sec += ( milliseconds / 1000 );
				milliseconds %= 1000;
				ts.tv_nsec = milliseconds * 1000000;
				if( !sem_timedwait( sema, &ts ) )
				{
					return errno = 0;
				}
				if( EINTR != errno )
				{
					break;
				}
				if( clock_gettime( CLOCK_REALTIME, &ts ) )
				{
					return errno;
				}
				now = ( ts.tv_sec * 1000 );
				now += ( ts.tv_nsec / 1000 );
				milliseconds = endTime - now;
				if( milliseconds < 0 )
				{
					return errno = ETIMEDOUT;
				}
			}
		}
		return errno;
	}
	return errno = EINVAL;
}

EXPORT int _XplSemaPost( _XplSema *sema )
{
	if( sema )
	{
		if( !sem_post( sema ) )
		{
			errno = 0;
		}
		else
		{
			fprintf( stderr, "sem_post() failure on thread %p, errno: %d\n", (void *)_GetPlatformThreadID(), errno );
		}
		return errno;
	}
	return errno = EINVAL;
}

EXPORT int _XplSemaValue( _XplSema *sema, int *value )
{
	if( sema )
	{
		if( !sem_getvalue( sema, value ) )
		{
			errno = 0;
		}
		return errno;
	}
	return errno = EINVAL;
}

EXPORT int _XplMutexInit( _XplMutex *mutex, XplBool recursive )
{
	if( mutex )
	{
		if( recursive )
		{
			pthread_mutex_t m = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

			memcpy( mutex, &m, sizeof( pthread_mutex_t ) );
			return 0;
		}
		if( !pthread_mutex_init( mutex, NULL ) )
		{
			errno = 0;
		}
		return errno;
	}
	return errno = EINVAL;
}

EXPORT int _XplMutexDestroy( _XplMutex *mutex )
{
	if( mutex )
	{
		errno = pthread_mutex_destroy( mutex );
		return errno;
	}
	return errno = EINVAL;
}

EXPORT int _XplMutexLock( _XplMutex *mutex )
{
	if( mutex )
	{
		do {
			errno = pthread_mutex_lock( mutex );
		} while (EAGAIN == errno);

		return errno;
	}
	return errno = EINVAL;
}

EXPORT int _XplMutexUnlock( _XplMutex *mutex )
{
	if( mutex )
	{
		errno = pthread_mutex_unlock( mutex );
		return errno;
	}
	return errno = EINVAL;
}

#endif	// LINUX

#ifdef DEBUG
EXPORT int __XplSemaInit( XplSemaphore *sema, int initialCount )
{
	sema->p = &sema->s;
	return _XplSemaInit( sema->p, initialCount );
}

EXPORT int __XplSemaDestroy( XplSemaphore *sema )
{
	DebugAssert( sema->p == &sema->s );
	sema->p = NULL;
	return _XplSemaDestroy( &sema->s );
}

EXPORT int __XplSemaWait( XplSemaphore *sema )
{
	DebugAssert( sema->p == &sema->s );
	return _XplSemaWait( sema->p );
}

EXPORT int __XplSemaTimedWait( XplSemaphore *sema, int64 milliseconds )
{
	DebugAssert( sema->p == &sema->s );
	return _XplSemaTimedWait( sema->p, milliseconds );
}

EXPORT int __XplSemaPost( XplSemaphore *sema )
{
	DebugAssert( sema->p == &sema->s );
	return _XplSemaPost( sema->p );
}

EXPORT int __XplSemaValue( XplSemaphore *sema, int *value )
{
	DebugAssert( sema->p == &sema->s );
	return _XplSemaValue( sema->p, value );
}

#endif


EXPORT int XplCondInit( XplCondVariable *var )
{
	if( var )
	{
		_XplSemaInit( &var->sema, 0 );
		_XplMutexInit( &var->mutex, FALSE );
		return errno = 0;
	}
	return errno = EINVAL;
}

EXPORT int XplCondDestroy( XplCondVariable *var )
{
	if( var )
	{
		_XplSemaDestroy( &var->sema );
		_XplMutexDestroy( &var->mutex );
		return errno = 0;
	}
	return errno = EINVAL;
}

EXPORT int XplCondWait( XplCondVariable *var, int64 milliseconds )
{
	if( var )
	{
		if( !_XplSemaTimedWait( &var->sema, milliseconds ) )
		{
			_XplMutexLock( &var->mutex );
			return errno = 0;
		}
		DebugAssert( ETIMEDOUT == errno );
		return ETIMEDOUT;
	}
	return errno = EINVAL;
}

EXPORT int XplCondSignal( XplCondVariable *var )
{
	return _XplSemaPost( &var->sema );
}

EXPORT int XplCondLock( XplCondVariable *var )
{
	return _XplMutexLock( &var->mutex );
}

EXPORT int XplCondUnlock( XplCondVariable *var )
{
	return _XplMutexUnlock( &var->mutex );
}

typedef struct _XplThread
{
	XplThreadStruct			userThread;
	struct _XplThread		*next;
	struct _XplThread		*globalNext;
	struct _XplThreadGroup	*threadGroup;
	int						(*userFunction)(XplThread);
	XplAtomic				consumers;
	_PlatformThreadID		id;
	XplCondVariable			signalLock;
	int						signalStack[MAX_SIGNAL_STACK];
	void					*signalContext[MAX_SIGNAL_STACK];
	void					*userData[MAX_USER_DATA];
}_XplThread;

typedef struct _XplThreadGroup
{
	XplThreadGroupStruct	userThreadGroup;
	struct _XplThreadGroup	*next;
	struct
	{
		XplLock				lock;
		_XplThread			*list;
	}thread;
	int						threads;
	size_t					stackSize;
	void					*userData[MAX_USER_DATA];
}_XplThreadGroup;

typedef struct _XplWork
{
	struct _XplWork	*next;
	_XplThread		*thread;
}_XplWork;

typedef enum
{
	TLibStateLoaded,		// not initialized yet
	TLibStateInitialized,	// initialized
	TLibStateRunning,		// Thread groups out
	TLibStateShutdown		// threads layer shutdown
}ThreadLibState;

#define CACHE_THREADS		1
#define CACHE_COUNT			8

typedef struct
{
	ThreadLibState	state;
	XplLock			lock;
	_XplThreadGroup	*groups;
	_XplThreadGroup	*staticGroup;
	_XplThread		*threadList;
#ifdef CACHE_THREADS
	struct
	{
		XplLock			lock;
		_XplThread		*list;
	}freeThreads;
#endif
	int				threads;	// sum of all groups
	int				lastThreadID;
	int				lastGroupID;
	struct
	{
		XplLock		lock;
		_XplWork	*list;
		_XplWork	**tail;
		_XplSema	semaphore;
		int			pending;
	}work;
	struct
	{
		XplLock		lock;
		uint32		total;
		uint32		idle;
		uint32		active;
		uint32		peak;
	}stats;
	XplCancelList	cancelList;
	char			*cacheText;
	char			*userRegistration[MAX_USER_DATA];

#ifdef GPROF
    struct itimerval itimer;
#endif
}XplThreadGlobals;

XplThreadGlobals XplThreads;

static void _InitializeThreadLibrary( void )
{
	static volatile int initState = 0;
	int len;
	char *tmp;

	if( !initState++ )
	{
		XplLockInit( &XplThreads.lock );
#ifdef CACHE_THREADS
		XplLockInit( &XplThreads.freeThreads.lock );
#endif
		XplThreads.state = TLibStateInitialized;
		XplLockInit( &XplThreads.work.lock );
		XplThreads.work.tail = &XplThreads.work.list;
		_XplSemaInit( &XplThreads.work.semaphore, 0 );
		XplLockInit( &XplThreads.stats.lock );
#ifdef GPROF
		getitimer(ITIMER_PROF, &XplThreads.itimer);
#endif
		XplCancelListInit( &XplThreads.cancelList );
		len = strlen( XplThreads.cacheText ) + strlen( "Thread Cache ()" ) + strlen( __FILE__ ) + 1;
		tmp = realloc( &XplThreads.cacheText, len );
		if( tmp ) {
			sprintf( tmp + strlen( XplThreads.cacheText ), "Thread Cache (%s)", __FILE__ );
		}
	}
}

// only called by memory manager when it closes completely
int XplThreadShutdown( void );
int XplThreadShutdown( void )
{
	_XplThread	*t;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			return 0;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			return 0;
	}

	XplLockAcquire( &XplThreads.lock );
	if( XplThreads.threads )
	{
		XplLockRelease( &XplThreads.lock );
		errno = EBUSY;
		return -1;
	}
	XplThreads.state = TLibStateShutdown;
	XplLockRelease( &XplThreads.lock );

	DebugAssert( !XplThreads.groups );
	DebugAssert( !XplThreads.staticGroup );
	DebugAssert( !XplThreads.threadList );

#ifdef CACHE_THREADS
	DebugAssert( !XplThreads.work.list );

	while( t = XplThreads.freeThreads.list )
	{
		XplThreads.freeThreads.list = t->globalNext;
		XplCondDestroy( &t->signalLock );
		free( t );
	}
#endif

	_XplSemaDestroy( &XplThreads.work.semaphore );
	free( XplThreads.cacheText );
	XplThreads.cacheText = __FILE__;

	return 0;
}

static int _XplThreadDestroy( _XplThread *t, const char *file, unsigned long line )
{
	_XplThread		**tp;
	_XplThreadGroup	*gr;

	if( t )
	{
		if( t->userThread.destroyCB )
		{
			t->userThread.destroyCB( &t->userThread );
		}
		if( gr = (_XplThreadGroup *)t->userThread.group )
		{
			XplLockAcquire( &gr->thread.lock );
			for(tp=&gr->thread.list;*tp;tp=&(*tp)->next)
			{
				if( *tp == t )
				{
					break;
				}
			}
			DebugAssert( *tp );	// thread not found
			*tp = t->next;
			gr->threads--;
			XplLockRelease( &gr->thread.lock );
		}
		t->threadGroup = NULL;
		XplLockAcquire( &XplThreads.lock );
		for(tp=&XplThreads.threadList;*tp;tp=&(*tp)->globalNext)
		{
			if( *tp == t )
			{
				break;
			}
		}
		DebugAssert( *tp );		// thread not found
		*tp = t->globalNext;
		XplThreads.threads--;

		if( gr == XplThreads.staticGroup )
		{
			// clean up static group if it is now empty
			XplLockAcquire( &gr->thread.lock );
			if( !gr->threads )
			{
				_XplThreadGroup **gp;

				gr->userThreadGroup.running = FALSE;
				XplThreads.staticGroup = NULL;
				XplLockRelease( &gr->thread.lock );
				for(gp=&XplThreads.groups;*gp;gp=&(*gp)->next)
				{
					if( *gp == gr )
					{
						break;
					}
				}
				DebugAssert( *gp );		// thread group not found
				*gp = gr->next;
				XplLockRelease( &XplThreads.lock );
				free( gr->userThreadGroup.identity );
				free( gr );
			}
			else
			{
				XplLockRelease( &gr->thread.lock );
				XplLockRelease( &XplThreads.lock );
			}
		}
		else
		{
			XplLockRelease( &XplThreads.lock );
		}
		return 0;
	}
	return -1;
}

static int64 _SystemTime( void )
{
	int64			microseconds;
#ifdef WIN32
	FILETIME		systemTime;

	// 100 nanosecond granularity
	// epoch is 00:00:00 Jan 1, 1601
	GetSystemTimeAsFileTime( &systemTime );
	microseconds = (int64)systemTime.dwLowDateTime;
	microseconds += (int64)systemTime.dwHighDateTime << 32;
	// convert to microseconds
	microseconds /= 10;
#else
	struct timespec	systemTime;

	// 1 nanosecond granularity
	// the resolution is system and clock dependent
	// epoch is 00:00:00 Jan 1, 1970
	if( clock_gettime( CLOCK_REALTIME, &systemTime ) )
	{
		return 0;
	}
	microseconds = (int64)systemTime.tv_sec * 1000000;
	microseconds += (int64)systemTime.tv_nsec / 1000;
#endif
	return microseconds;
}

// Maximum idle time for a thread is 5 seconds
#define XPL_THREAD_MAX_IDLE		5000000

#ifdef GPROF
static void __xpl_thread__( struct itimerval *itimer )
#else
static void __xpl_thread__( void )
#endif
{
	_XplWork			*work;
	_XplThread			*thread;
	_PlatformThreadID 	id = _GetPlatformThreadID();
	int64	idleStart, idleStop;

#ifdef GPROF
    setitimer(ITIMER_PROF, &XplThreads.itimer, NULL);
#endif

	for(;;)
	{
		idleStart = _SystemTime();
		_XplSemaWait( &XplThreads.work.semaphore );
		idleStop = _SystemTime();

		XplLockAcquire( &XplThreads.work.lock );
		if( work = XplThreads.work.list )
		{
			XplThreads.work.pending--;
			if( !(XplThreads.work.list = work->next ) )
			{
				XplThreads.work.tail = &XplThreads.work.list;
			}
		}
		XplLockRelease( &XplThreads.work.lock );

		XplLockAcquire( &XplThreads.stats.lock );
		if( !work )
		{
			// killing
			XplThreads.stats.total--;
			XplLockRelease( &XplThreads.stats.lock );
			return;
		}
		XplThreads.stats.idle--;
		XplThreads.stats.active++;
		if( XplThreads.stats.active > XplThreads.stats.peak )
		{
			XplThreads.stats.peak = XplThreads.stats.active;
		}
		XplLockRelease( &XplThreads.stats.lock );

		thread = work->thread;
		free( work );

		thread->id = id;
		thread->userThread.running = TRUE;
		thread->userThread.exitCode = thread->userFunction( (XplThread)thread );
		thread->userThread.running = FALSE;
		XplThreadSignal( &thread->userThread, SIGTERM, NULL );
		_XplThreadDestroy( thread, __FILE__, __LINE__ );
		XplThreadFree( (XplThread *)&thread );

		XplLockAcquire( &XplThreads.stats.lock );
		XplThreads.stats.active--;
		if( idleStop && idleStart )
		{
			if( (idleStop - idleStart) >= XPL_THREAD_MAX_IDLE )
			{
				// was idle for more than 5 seconds
				// killing
				XplThreads.stats.total--;
				XplLockRelease( &XplThreads.stats.lock );
				return;
			}
		}
		XplThreads.stats.idle++;
		XplLockRelease( &XplThreads.stats.lock );
	}
}

static void __xpl_single_thread__( _XplThread *thread )
{
	_PlatformThreadID 	id = _GetPlatformThreadID();

#ifdef GPROF
    setitimer(ITIMER_PROF, &XplThreads.itimer, NULL);
#endif

	XplLockAcquire( &XplThreads.stats.lock );
	XplThreads.stats.total++;
	XplThreads.stats.active++;
	if( XplThreads.stats.active > XplThreads.stats.peak )
	{
		XplThreads.stats.peak = XplThreads.stats.active;
	}
	XplLockRelease( &XplThreads.stats.lock );

	thread->id = id;
	thread->userThread.running = TRUE;
	thread->userThread.exitCode = thread->userFunction( (XplThread)thread );
	thread->userThread.running = FALSE;
	XplThreadSignal( &thread->userThread, SIGTERM, NULL );
	_XplThreadDestroy( thread, __FILE__, __LINE__ );
	XplThreadFree( (XplThread *)&thread );

	XplLockAcquire( &XplThreads.stats.lock );
	XplThreads.stats.total--;
	XplThreads.stats.active--;
	XplLockRelease( &XplThreads.stats.lock );
}

#ifdef WIN32

static int _CreateThread( _XplThread *thread )
{
	HANDLE	threadHandle;
	DWORD	threadID;

	if( thread )
	{
		threadHandle = CreateThread( NULL, thread->threadGroup->stackSize * 1024, (LPTHREAD_START_ROUTINE)__xpl_single_thread__, thread, 0, &threadID );
	}
	else
	{
		threadHandle = CreateThread( NULL, THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)__xpl_thread__, NULL, 0, &threadID );
	}

	if( threadHandle )
	{
		CloseHandle( threadHandle );
		return 0;
	}
	return -1;
}

#endif	// WIN32

#if defined(LINUX) || defined(S390RH) || defined(MACOSX)

typedef void *(*PThreadFunc)( void * );

static int _CreateThread( _XplThread *thread )
{
	pthread_attr_t	threadAttr;
	pthread_t		id;

	pthread_attr_init(&threadAttr);
	pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_DETACHED);

	if( thread )
	{

		pthread_attr_setstacksize(&threadAttr, thread->threadGroup->stackSize * 1024);
		if( !pthread_create(&id, &threadAttr, (PThreadFunc)__xpl_single_thread__, thread ) )
		{
			pthread_attr_destroy( &threadAttr );
			return 0;
		}
	}
	else
	{
		pthread_attr_setstacksize(&threadAttr, THREAD_STACK_SIZE);
		if( !pthread_create(&id, &threadAttr, (PThreadFunc)__xpl_thread__, NULL ) )
		{
			pthread_attr_destroy( &threadAttr );
			return 0;
		}
	}

	pthread_attr_destroy( &threadAttr );
	return -1;
}

#endif	// LINUX

static void _Schedule( _XplThread *thread )
{
	_XplWork	*work;
	int			pending;

	if( thread->threadGroup->stackSize )
	{
		// Custom thread stack size
		do
		{
			if( !_CreateThread( thread ) )
			{
				return;
			}
			// keep trying to create the thread, custom threads don't use
			// the work queue.
			XplDelay( 500 );
		}while( thread->threadGroup->userThreadGroup.running );
		return;
	}
	work = malloc( sizeof( _XplWork ) );
	work->thread = thread;
	work->next = NULL;
	XplLockAcquire( &XplThreads.work.lock );
	*XplThreads.work.tail = work;
	XplThreads.work.tail = &work->next;
	XplThreads.work.pending++;
	pending = XplThreads.work.pending;
	XplLockRelease( &XplThreads.work.lock );

	XplLockAcquire( &XplThreads.stats.lock );
	while( pending >= XplThreads.stats.idle )
	{
		XplThreads.stats.total++;
		XplThreads.stats.idle++;
		XplLockRelease( &XplThreads.stats.lock );

		if( !_CreateThread( NULL ) )
		{
			_XplSemaPost( &XplThreads.work.semaphore );
			return;
		}
		XplLockAcquire( &XplThreads.stats.lock );
		pending = XplThreads.work.pending;
		XplLockRelease( &XplThreads.work.lock );

		XplLockAcquire( &XplThreads.stats.lock );
		XplThreads.stats.total--;
		XplThreads.stats.idle--;
	}
	XplLockRelease( &XplThreads.stats.lock );

	_XplSemaPost( &XplThreads.work.semaphore );
}

static XplThreadGroup __XplThreadGroupCreate( const char *identity, XplBool locked, const char *file, unsigned long line )
{
	_XplThreadGroup	*group;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			_InitializeThreadLibrary();
			if( TLibStateLoaded == XplThreads.state )
			{
				errno = ENODEV;
				return NULL;
			}
			break;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return NULL;
	}

	errno = ENOMEM;
	if( group = malloc( sizeof( _XplThreadGroup ) ) )
	{
		if( identity )
		{
			group->userThreadGroup.identity = strdup( identity );
		}
		else
		{
			group->userThreadGroup.identity = strdup( "[static]" );
		}
		XplLockInit( &group->thread.lock );
		group->userThreadGroup.running = TRUE;
		if( !locked )	XplLockAcquire( &XplThreads.lock );
		group->userThreadGroup.id = ++XplThreads.lastGroupID;
		group->next = XplThreads.groups;
		XplThreads.groups = group;
		if( !locked )	XplLockRelease( &XplThreads.lock );
	}
	return (XplThreadGroup)group;
}

EXPORT XplThreadGroup _XplThreadGroupCreate( const char *identity, const char *file, unsigned long line )
{
	return __XplThreadGroupCreate( identity, FALSE, file, line );
}

EXPORT int _XplThreadGroupDestroy( XplThreadGroup *group, const char *file, unsigned long line )
{
	_XplThreadGroup	*gr, **gp;
	_XplThread		*t;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	errno = EINVAL;
	if( group && ( gr = (_XplThreadGroup *)*group ) )
	{
		gr->userThreadGroup.running = FALSE;

		XplLockAcquire( &gr->thread.lock );
		for(t=gr->thread.list;t;t=t->next)
		{
			XplThreadSignal( (XplThread)t, SIGTERM, NULL );
		}

		while( gr->threads )
		{
			XplLockRelease( &gr->thread.lock );
			XplDelay( 500 );
			XplLockAcquire( &gr->thread.lock );
		}
		XplLockRelease( &gr->thread.lock );

		XplLockAcquire( &XplThreads.lock );
		for(gp=&XplThreads.groups;*gp;gp=&(*gp)->next)
		{
			if( *gp == gr )
			{
				break;
			}
		}
		DebugAssert( *gp );		// thread group not found
		*gp = gr->next;
		XplLockRelease( &XplThreads.lock );

		free( gr->userThreadGroup.identity );
		free( *group );
		*group = NULL;
	}
	return -1;
}

EXPORT int _XplThreadGroupStackSize( XplThreadGroup group, size_t sizeInK, const char *file, unsigned long line )
{
	_XplThreadGroup	*gr;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	errno = EINVAL;
	if( group && ( gr = (_XplThreadGroup *)group ) )
	{
		gr->stackSize = sizeInK;
		return 0;
	}
	return -1;
}

static void _LinkThread( _XplThread *thread, _XplThreadGroup *gr )
{
	XplLockAcquire( &gr->thread.lock );
	thread->next = gr->thread.list;
	gr->thread.list = thread;
	gr->threads++;
	thread->threadGroup = gr;
	XplLockRelease( &gr->thread.lock );

	XplLockAcquire( &XplThreads.lock );
	thread->userThread.id = ++XplThreads.lastThreadID;
	thread->globalNext = XplThreads.threadList;
	XplThreads.threadList = thread;
	XplThreads.threads++;
	XplLockRelease( &XplThreads.lock );
}

EXPORT int _XplThreadStart( XplThreadGroup group, int (*userFunction)( XplThread ), void *context, XplThread *threadP, const char *file, unsigned long line )
{
	_XplThread		*thread;
	_XplThreadGroup	*gr = (_XplThreadGroup *)group;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			_InitializeThreadLibrary();
			if( TLibStateLoaded == XplThreads.state )
			{
				errno = ENODEV;
				return -1;
			}
			break;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( !userFunction )
	{
		errno = EINVAL;
		return -1;
	}

	if( !gr )
	{
		XplLockAcquire( &XplThreads.lock );
		if( !( gr = XplThreads.staticGroup ) )
		{
			gr = (_XplThreadGroup *)__XplThreadGroupCreate( NULL, TRUE, file, line );
			XplThreads.staticGroup = gr;
		}
		XplLockRelease( &XplThreads.lock );
	}
	if( !gr )
	{
		return -1;
	}
#ifdef CACHE_THREADS
	XplLockAcquire( &XplThreads.freeThreads.lock );
	if( thread = XplThreads.freeThreads.list )
	{
		XplThreads.freeThreads.list = thread->globalNext;
		XplSafeWrite( thread->consumers, 1 );
	}
	XplLockRelease( &XplThreads.freeThreads.lock );
#else
	thread = NULL;
#endif

	if( !thread )
	{
		errno = ENOMEM;
		if( thread = malloc( sizeof( _XplThread ) ) )
		{
			XplSafeInit( thread->consumers, 1 );
			XplCondInit( &thread->signalLock );
		}
	}
	if( thread )
	{
		thread->userFunction = userFunction;
		thread->userThread.group = (XplThreadGroup)gr;
		thread->userThread.context = context;
		thread->userThread.running = TRUE;
		memset( &thread->signalStack[0], 0, sizeof( int ) * MAX_SIGNAL_STACK );
		memset( &thread->signalContext[0], 0, sizeof( void * ) * MAX_SIGNAL_STACK );
		if( threadP )
		{
			XplSafeIncrement( thread->consumers );
			*threadP = (XplThread)thread;
		}
		_LinkThread( thread, gr );
		_Schedule( thread );
		return 0;
	}
	return -1;
}

EXPORT int _XplThreadWrap( XplThread *threadP, const char *file, unsigned long line )
{
	_XplThread		*thread;
	_XplThreadGroup	*gr;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			_InitializeThreadLibrary();
			if( TLibStateLoaded == XplThreads.state )
			{
				errno = ENODEV;
				return -1;
			}
			break;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( !threadP )
	{
		errno = EINVAL;
		return -1;
	}

	XplLockAcquire( &XplThreads.lock );
	if( !( gr = XplThreads.staticGroup ) )
	{
		gr = (_XplThreadGroup *)__XplThreadGroupCreate( NULL, TRUE, file, line );
		XplThreads.staticGroup = gr;
	}
	XplLockRelease( &XplThreads.lock );
	if( !gr )
	{
		return -1;
	}
	errno = ENOMEM;
	if( thread = malloc( sizeof( _XplThread ) ) )
	{
		XplSafeInit( thread->consumers, 1 );
		thread->userFunction = NULL;
		thread->userThread.group = (XplThreadGroup)gr;
		thread->userThread.context = NULL;
		thread->userThread.running = TRUE;
		XplCondInit( &thread->signalLock );
		memset( &thread->signalStack[0], 0, sizeof( int ) * MAX_SIGNAL_STACK );
		memset( &thread->signalContext[0], 0, sizeof( void * ) * MAX_SIGNAL_STACK );
		*threadP = (XplThread)thread;
		_LinkThread( thread, gr );

		XplLockAcquire( &XplThreads.stats.lock );
		XplThreads.stats.active++;
		if( XplThreads.stats.active > XplThreads.stats.peak )
		{
			XplThreads.stats.peak = XplThreads.stats.active;
		}
		XplLockRelease( &XplThreads.stats.lock );
		thread->id = _GetPlatformThreadID();
		thread->userThread.running = TRUE;

		return 0;
	}
	return -1;
}

EXPORT int _XplThreadWrapEnd( XplThread *threadP, const char *file, unsigned long line )
{
	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( !threadP || !*threadP )
	{
		errno = EINVAL;
		return -1;
	}

	_XplThreadDestroy( (_XplThread *)*threadP, __FILE__, __LINE__ );
	XplThreadFree( threadP );

	XplLockAcquire( &XplThreads.stats.lock );
	XplThreads.stats.active--;
	XplLockRelease( &XplThreads.stats.lock );
	return 0;
}


EXPORT int _XplThreadFree( XplThread *thread, const char *file, unsigned long line )
{
	_XplThread	*t;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( thread && *thread )
	{
		t = (_XplThread *)*thread;
		if( !XplSafeDecrement( t->consumers ) )
		{
#ifdef CACHE_THREADS
			while( -1 != XplThreadCatch( *thread, NULL, 0 ) )
				;
			t->userFunction = NULL;
			t->userThread.group = NULL;
			t->userThread.context = NULL;
			t->userThread.running = FALSE;
			memset( &t->signalStack[0], 0, sizeof( int ) * MAX_SIGNAL_STACK );
			memset( &t->signalContext[0], 0, sizeof( void * ) * MAX_SIGNAL_STACK );

			XplLockAcquire( &XplThreads.freeThreads.lock );
			t->globalNext = XplThreads.freeThreads.list;
			XplThreads.freeThreads.list = t;
			XplLockRelease( &XplThreads.freeThreads.lock );
#else
			XplCondDestroy( &t->signalLock );
			free( *thread );
#endif
		}
		*thread = NULL;
		return 0;
	}
	errno = EINVAL;
	return -1;
}

static _XplThread *_CurrentThread( void )
{
	_XplThread			*t;
	_PlatformThreadID	id;

	id = _GetPlatformThreadID();

	XplLockAcquire( &XplThreads.lock );
	for(t=XplThreads.threadList;t;t=t->globalNext)
	{
		if( t->id == id )
		{
			break;
		}
	}
	XplLockRelease( &XplThreads.lock );
#if 0
	if( !t )
	{
		fprintf( stderr, "*** Convert this binary to use XplMain() or XplServiceMain() ***\n" );
	}
//	DebugAssert(t);	// current thread not found
#endif
	return t;
}

EXPORT XplThread XplGetThread( void )
{
	return (XplThread)_CurrentThread();
}

EXPORT XplThreadGroup XplGetThreadGroup( XplThread thread )
{
	_XplThread	*t;

	if( thread )
	{
		return thread->group;
	}
	else
	{
		if( t = _CurrentThread() )
		{
			return t->userThread.group;
		}
	}
	return NULL;
}

EXPORT XplThreadID XplGetThreadID( void )
{
	XplThread	t;

	if( t = XplGetThread() )
	{
		return t->id;
	}
	return -1;
}

EXPORT XplThreadGroupID XplGetThreadGroupID( XplThread thread )
{
	XplThreadGroup	g;

	if( g = XplGetThreadGroup( thread ) )
	{
		return g->id;
	}
	return -1;
}

EXPORT int XplThreadSignal( XplThread thread, int sig, void *context )
{
	_XplThread	*t;
	int			l;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( !sig )
	{
		errno = EINVAL;
		return -1;
	}
	if( !( t = (_XplThread *)thread ) )
	{
		t = _CurrentThread();
		if( !t )
		{
			errno = ENOSYS;
			return -1;
		}
	}
	XplCondLock( &t->signalLock );
	for(l=0;l<MAX_SIGNAL_STACK;l++)
	{
		if( !t->signalStack[l] )
		{
			t->signalStack[l] = sig;
			t->signalContext[l] = context;
			XplCondUnlock( &t->signalLock );
			XplCondSignal( &t->signalLock );
			return 0;
		}
	}
	XplCondUnlock( &t->signalLock );
	errno = ENOSPC;
	return -1;
}

EXPORT int XplThreadGroupSignal( XplThreadGroup group, int sig, void *context )
{
	_XplThreadGroup	*gr;
	_XplThread		*t;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( !sig )
	{
		errno = EINVAL;
		return -1;
	}
	if( !( gr = (_XplThreadGroup *)group ) )
	{
		gr = (_XplThreadGroup *)XplGetThreadGroup( NULL );
	}
	DebugAssert( gr );	// NULL thread group

	XplLockAcquire( &gr->thread.lock );
	for(t=gr->thread.list;t;t=t->next)
	{
		XplThreadSignal( (XplThread)t, sig, context );
	}
	XplLockRelease( &gr->thread.lock );

	return 0;
}

// called with sync object locked and unlocks the object
static int _GetSignal( _XplThread *t, void **context )
{
	int		sig = 0;

	if( t->signalStack[0] )
	{
		sig = t->signalStack[0];
		if( context )
		{
			*context = t->signalContext[0];
		}
		memmove( &t->signalStack[0], &t->signalStack[1], sizeof( int ) * ( MAX_SIGNAL_STACK - 1 ) );
		memmove( &t->signalContext[0], &t->signalContext[1], sizeof( void * ) * ( MAX_SIGNAL_STACK - 1 ) );
		t->signalStack[MAX_SIGNAL_STACK-1] = 0;
		t->signalContext[MAX_SIGNAL_STACK-1] = NULL;
	}
	XplCondUnlock( &t->signalLock );
	return sig;
}

EXPORT int XplThreadJoin( XplThread thread, int milliseconds )
{
	_XplThread	*t;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( !( t = (_XplThread *)thread ) )
	{
		errno = EINVAL;
		return -1;
	}
	for(;;)
	{
		switch( XplCondWait( &t->signalLock, milliseconds ) )
		{
			case 0:	// we were signaled, the object is locked
				if( t->userThread.running )
				{
					// let something else handle this signal
					XplCondUnlock( &t->signalLock );
					XplCondSignal( &t->signalLock );
				}
				else
				{
					XplCondUnlock( &t->signalLock );
					errno = 0;
					return t->userThread.exitCode;
				}
				break;

			case ETIMEDOUT:
				errno = ETIMEDOUT;
				return -1;
		}
	}
}

EXPORT int XplThreadCatch( XplThread thread, void **context, int64 milliseconds )
{
	_XplThread	*t;
	int			sig;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( !( t = (_XplThread *)thread ) )
	{
		t = _CurrentThread();
		if( !t )
		{
			errno = ENOSYS;
			return -1;
		}
	}

	switch( XplCondWait( &t->signalLock, milliseconds ) )
	{
		case 0:	// we were signaled, the object is locked
			sig = _GetSignal( t, context );	// unlocks the object
			DebugAssert( sig );		// signal 0
			errno = 0;
			return sig;

		case ETIMEDOUT:
			errno = ETIMEDOUT;
			break;
	}
	return -1;
}

EXPORT int XplThreadStats( uint32 *total, uint32 *idle, uint32 *active, uint32 *peak )
{
	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	XplLockAcquire( &XplThreads.stats.lock );
	if( total )		*total = XplThreads.stats.total;
	if( idle )		*idle = XplThreads.stats.idle;
	if( active )	*active = XplThreads.stats.active;
	if( peak )		*peak = XplThreads.stats.peak;
	XplLockRelease( &XplThreads.stats.lock );
	return 0;
}

EXPORT int XplThreadCount( XplThreadGroup pub )
{
	_XplThreadGroup	*gr;
	int				threads;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( pub )
	{
		gr = (_XplThreadGroup *)pub;
		XplLockAcquire( &gr->thread.lock );
		threads = gr->threads;
		XplLockRelease( &gr->thread.lock );
	}
	else
	{
		XplLockAcquire( &XplThreads.lock );
		threads = XplThreads.threads;
		XplLockRelease( &XplThreads.lock );
	}
	return threads;
}

// helper function
EXPORT XplBool XplThreadTerminated( XplThread thread, void *context, int64 milliseconds )
{
	switch ( XplThreadCatch( thread, context, milliseconds ) )
	{
		case SIGTERM:
			XplThreadSignal(thread, SIGTERM, NULL);
			return TRUE;

		default:
			break;
	}
	return FALSE;
}

EXPORT int XplThreadRegisterData( char *name )
{
	int		l;
	char	*userName;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			_InitializeThreadLibrary();
			if( TLibStateLoaded == XplThreads.state )
			{
				errno = ENODEV;
				return -1;
			}
			break;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( !name )
	{
		errno = EINVAL;
		return -1;
	}
	userName = strdup( name );
	if( !userName ) {
		errno = ENOMEM;
		return -1;
	}
	XplLockAcquire( &XplThreads.lock );
	for(l=0;l<MAX_USER_DATA;l++)
	{
		if( XplThreads.userRegistration[l] )
		{
			if( !strcmp( userName, XplThreads.userRegistration[l] ) )
			{
				XplLockRelease( &XplThreads.lock );
				if( userName ) {
					free( userName );
					userName = NULL;
				}
				return l;
			}
		}
	}
	for(l=0;l<MAX_USER_DATA;l++)
	{
		if( !XplThreads.userRegistration[l] )
		{
			XplThreads.userRegistration[l] = name;
			XplLockRelease( &XplThreads.lock );
			return l;
		}
	}
	XplLockRelease( &XplThreads.lock );
	errno = ENOMEM;
	return -1;
}

static int _ValidateSlot( int slot )
{
	if( ( slot < 0 ) || ( slot >= MAX_USER_DATA ) )
	{
		return -1;
	}
	XplLockAcquire( &XplThreads.lock );
	if( !XplThreads.userRegistration[slot] )
	{
		XplLockRelease( &XplThreads.lock );
		return -1;
	}
	XplLockRelease( &XplThreads.lock );
	return 0;
}

EXPORT int XplThreadSetData( XplThread thread, int slot, void *data )
{
	_XplThread		*t;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( _ValidateSlot( slot ) )
	{
		errno = EINVAL;
		return -1;
	}
	if( !( t = (_XplThread *)thread ) )
	{
		t = _CurrentThread();
		if( !t )
		{
			errno = ENOSYS;
			return -1;
		}
	}
	t->userData[slot] = data;
	return 0;
}

EXPORT int XplThreadGetData( XplThread thread, int slot, void **data )
{
	_XplThread		*t;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( !data || _ValidateSlot( slot ) )
	{
		errno = EINVAL;
		return -1;
	}
	if( !( t = (_XplThread *)thread ) )
	{
		t = _CurrentThread();
		if( !t )
		{
			errno = ENOSYS;
			return -1;
		}
	}
	*data = t->userData[slot];
	return 0;
}

EXPORT int XplThreadGroupSetData( XplThreadGroup group, int slot, void *data )
{
	_XplThreadGroup	*gr;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( _ValidateSlot( slot ) )
	{
		errno = EINVAL;
		return -1;
	}
	if( !( gr = (_XplThreadGroup *)group ) )
	{
		gr = (_XplThreadGroup *)XplGetThreadGroup( NULL );
	}
	DebugAssert( gr );	// NULL thread group
	gr->userData[slot] = data;
	return 0;
}

EXPORT int XplThreadGroupGetData( XplThreadGroup group, int slot, void **data )
{
	_XplThreadGroup	*gr;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( !data || _ValidateSlot( slot ) )
	{
		errno = EINVAL;
		return -1;
	}
	if( !( gr = (_XplThreadGroup *)group ) )
	{
		gr = (_XplThreadGroup *)XplGetThreadGroup( NULL );
	}
	*data = gr->userData[slot];
	return 0;
}

EXPORT void _XplCancelInit( XplCancelList *list, XplCancelPoint *point )
{
	// This should be called with a stack buffer, there is no way it should be NULL
	DebugAssert( list && point );

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			_InitializeThreadLibrary();
			if( TLibStateLoaded == XplThreads.state )
			{
				errno = ENODEV;
				return;
			}
			break;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return;
	}

	point->thread = XplGetThread();
	point->cancelled = FALSE;
	point->context = NULL;

	XplLockAcquire( &list->lock );
	point->next = list->list;
	list->list = point;
	XplLockRelease( &list->lock );
}

EXPORT void XplCancelInit( XplCancelPoint *point )
{
	// library state check is in _XplCancelInit
	_XplCancelInit( &XplThreads.cancelList, point );
}

EXPORT void XplCancelListInit( XplCancelList *list )
{
	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			_InitializeThreadLibrary();
			if( TLibStateLoaded == XplThreads.state )
			{
				errno = ENODEV;
				return;
			}
			break;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return;
	}

	XplLockInit( &list->lock );
	list->list = NULL;
}

EXPORT int _XplCancelDestroy( XplCancelList *list, XplCancelPoint *point )
{
	XplCancelPoint **pp;

	// This should be called with a stack buffer, there is no way it should be NULL
	DebugAssert( point );

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			_InitializeThreadLibrary();
			if( TLibStateLoaded == XplThreads.state )
			{
				errno = ENODEV;
				return -1;
			}
			break;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( !list )
	{
		errno = EINVAL;
		return -1;
	}

	XplLockAcquire( &list->lock );
	for(pp=&list->list;*pp;pp=&(*pp)->next)
	{
		if( *pp == point )
		{
			*pp = point->next;
			XplLockRelease( &list->lock );
			point->next = NULL;
			point->thread = NULL;
			point->context = NULL;
			return 0;
		}
	}
	XplLockRelease( &list->lock );
	DebugAssert( 0 );	// invalid cancel point
	return -1;
}

EXPORT void XplCancelDestroy( XplCancelPoint *point )
{
	// library state check is in _XplCancelDestroy
	_XplCancelDestroy( &XplThreads.cancelList, point );
}

EXPORT int _XplCancelThread( XplCancelList *list, XplThread thread, void *context )
{
	XplCancelPoint	*p;

	switch( XplThreads.state )
	{
		case TLibStateLoaded:
			errno = ENODEV;
			return -1;

		case TLibStateInitialized:
		case TLibStateRunning:
			break;

		case TLibStateShutdown:
			errno = ENOSYS;
			return -1;
	}

	if( !list )
	{
		errno = EINVAL;
		return -1;
	}
	XplLockAcquire( &list->lock );
	for(p=list->list;p;p=p->next)
	{
		if( thread )
		{
			if( p->thread == thread )
			{
				p->cancelled = TRUE;
				p->context = context;
				XplLockRelease( &list->lock );
				errno = 0;
				return 0;
			}
		}
		else
		{
			p->cancelled = TRUE;
			p->context = context;
		}
	}
	XplLockRelease( &list->lock );
	if( !thread )
	{
		errno = 0;
		return 0;
	}
	errno = ENOENT;
	return -1;
}

EXPORT int XplCancelThread( XplThread thread, void *context )
{
	// library state check is in _XplCancelThread
	return _XplCancelThread( &XplThreads.cancelList, thread, context );
}

// thread safe versions of time functions

#ifdef __WATCOMC__
EXPORT char *asctime_r( const struct tm *timeptr, char *buf )
{
	return _asctime( timeptr, buf );
}
#endif

#ifdef _MSC_VER
EXPORT char *asctime_r( const struct tm *timeptr, char *buf )
{
	char buffer[1024];
	asctime_s( buffer, sizeof( buffer ), timeptr );
	strcpy( buf, buffer );
	return buf;
}
#endif

#ifdef __WATCOMC__
EXPORT char *ctime_r( const time_t *timer, char *buf )
{
	return _ctime( timer, buf );
}
#endif

#ifdef _MSC_VER
EXPORT char *ctime_r( const time_t *timer, char *buf )
{
	char buffer[1024];
	ctime_s( buffer, sizeof( buffer ), timer );
	strcpy( buf, buffer );
	return buf;
}
#endif

#ifdef __WATCOMC__
EXPORT struct tm *gmtime_r( const time_t *timer, struct tm *tmbuf )
{
	return _gmtime( timer, tmbuf );
}
#endif

#ifdef _MSC_VER
EXPORT struct tm *gmtime_r( const time_t *timer, struct tm *tmbuf )
{
	gmtime_s( tmbuf, timer );
	return tmbuf;
}
#endif

#ifdef __WATCOMC__
EXPORT struct tm *localtime_r( const time_t *timer, struct tm *tmbuf )
{
	return _localtime( timer, tmbuf );
}
#endif

#ifdef _MSC_VER
EXPORT struct tm *localtime_r( const time_t *timer, struct tm *tmbuf )
{
	localtime_s( tmbuf, timer );
	return tmbuf;
}
#endif

