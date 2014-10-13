 /*
 This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "Ntifs.h"
#include "Ntddmou.h"
#include "Ntddkbd.h"
#include "Kbdmou.h"




#define KBDCLASS_CONNECT_REQUEST 0x0B0203
#define MOUCLASS_CONNECT_REQUEST 0x0F0203
#define MOU_STRING_INC 0x14
#define KBD_STRING_INC 0x15




struct DEVOBJ_EXTENSION_FIX
{
	USHORT type;
	USHORT size;
	PDEVICE_OBJECT devObj;
	ULONGLONG PowerFlags;
	void *Dope;
	ULONGLONG ExtensionFlags;
	void *DeviceNode;
	PDEVICE_OBJECT AttachedTo;
};

/*
These routines are resolved after an adddevice call to mouclass and kbdclass.

MouClass and KbdClass then respond with a KBDCLASS_CONNECT_REQUEST or MOUCLASS_CONNECT_REQUEST as an internal 
device request, giving us the linear address of the input functions. We then call them just like a real miniport driver
would ;p
*/
typedef void(__fastcall *MouseServiceDpc)(PDEVICE_OBJECT mou, PMOUSE_INPUT_DATA a1, PMOUSE_INPUT_DATA a2, PULONG a3);
typedef void(__fastcall *KeyboardServiceDpc)(PDEVICE_OBJECT kbd, PKEYBOARD_INPUT_DATA a1, PKEYBOARD_INPUT_DATA a2, PULONG a3);


typedef NTSTATUS(__fastcall *MouseAddDevice)(PDRIVER_OBJECT a1, PDEVICE_OBJECT a2);
typedef NTSTATUS(__fastcall *KeyboardAddDevice)(PDRIVER_OBJECT a1, PDEVICE_OBJECT a2);
typedef NTSTATUS(__fastcall *MmCopyVirtualMemory)(PEPROCESS a1, void *a2, PEPROCESS *a3, void *a4, ULONGLONG a5, KPROCESSOR_MODE a6, ULONG *a7);
typedef NTSTATUS(__fastcall *KbdclassRead)(PDEVICE_OBJECT device, PIRP irp);
typedef NTSTATUS(__fastcall *MouclassRead)(PDEVICE_OBJECT device, PIRP irp);
typedef NTSTATUS(__fastcall *kbdinput)(void *a1, void *a2, void *a3, void *a4, void *a5);
typedef NTSTATUS(__fastcall *mouinput)(void *a1, void *a2, void *a3, void *a4, void *a5);
typedef NTSTATUS(__fastcall *NtQuerySystemInformation)(ULONG infoclass, void *buffer, ULONG infolen, ULONG *plen);
typedef char*(_fastcall *PsGetProcessImageFileName)(PEPROCESS target);
typedef void*(_fastcall *PsGetProcessPeb)(PEPROCESS a1);
typedef void*(_fastcall *PsGetProcessWow64Process)(PEPROCESS a1);

extern NTSTATUS SystemRoutine();


PDEVICE_OBJECT mouTarget;
PDEVICE_OBJECT kbdTarget;
ULONG mouId=0;
ULONG kbdId=0;
PEPROCESS targetProcess=NULL;
PEPROCESS currentProcess=NULL;
char KEY_DATA[128];
char MOU_DATA[5];

MOUSE_INPUT_DATA mdata;
KEYBOARD_INPUT_DATA kdata;

KbdclassRead KbdClassReadRoutine;
MouclassRead MouClassReadRoutine;
MouseServiceDpc MouseDpcRoutine;
KeyboardServiceDpc KeyboardDpcRoutine;
MmCopyVirtualMemory MmCopyVirtualMemoryRoutine;
NtQuerySystemInformation ZwQuerySystemInformation;
kbdinput KeyboardInputRoutine=NULL;
mouinput MouseInputRoutine=NULL;
PsGetProcessImageFileName PsGetImageName;
PsGetProcessPeb PsGetPeb64;
PsGetProcessWow64Process PsGetPeb32;

PKEYBOARD_INPUT_DATA mjRead=NULL;
PMOUSE_INPUT_DATA mouIrp=NULL;


/*
This function calls the KeyboardClassServiceCallback routine given to us after we added our device,
via the internal device request. Before branching to the actual input routine, we must raise 
our IRQL to dispatch level because we are pretending that it is a dpcforisr routine, not to mention 
these routines acquire dispatch level spinlocks without checking or modifying the previous IRQL.
*/
void SynthesizeKeyboard(PKEYBOARD_INPUT_DATA a1)
{
	KIRQL irql;
	char *endptr;
	ULONG fill=1;


	endptr=(char*)a1;

	endptr+=sizeof(KEYBOARD_INPUT_DATA);

	a1->UnitId=kbdId;

	//huehuehue
	KeRaiseIrql(DISPATCH_LEVEL,&irql);

	KeyboardDpcRoutine(kbdTarget,a1,(PKEYBOARD_INPUT_DATA)endptr,&fill);

	KeLowerIrql(irql);

}
/*
This function calls the MouseClassServiceCallback routine given to us after we added our device,
via the internal device request. Before branching to the actual input routine, we must raise 
our IRQL to dispatch level because we are pretending that it is a dpcforisr routine, not to mention 
these routines acquire dispatch level spinlocks without checking or modifying the previous IRQL.
*/
void SynthesizeMouse(PMOUSE_INPUT_DATA a1)
{
	KIRQL irql;
	char *endptr;
	ULONG fill=1;

	endptr=(char*)a1;

	endptr+=sizeof(MOUSE_INPUT_DATA);

	a1->UnitId=mouId;

	//huehuehue
	KeRaiseIrql(DISPATCH_LEVEL,&irql);

	MouseDpcRoutine(mouTarget,a1,(PMOUSE_INPUT_DATA)endptr,&fill);

	KeLowerIrql(irql);

}

/*
This function asynchronously obtains the up/down keystate of the corresponding scan code.
This global array is updated each time the user presses a key, it is updated via the hijacked 
InputApc queued to the CSRSS keyboard listener whenever the completion code calls IoCompleteRequest
*/
int GetKeyState(char scan)
{
	if(KEY_DATA[scan-1]) return 1;

	return 0;
}

/*
This function asynchronously obtains the up/down mouse button state of the corresponding mouse button.
This global array is updated each time the user presses a mouse button, it is updated via the hijacked 
InputApc queued to the CSRSS mouse listener whenever the completion code calls IoCompleteRequest
*/
int GetMouseState(int key)
{
	if(MOU_DATA[key]) return 1;

	return 0;
}

/*
This is just an easy to use wrapper around MmCopyVirtualMemory, as you can see a TargetProcess is required
or the call will just fail. Since MmCopyVirtualMemory contains it's own exception handling and data probes, 
the call will fail if the source is bogus.
*/
NTSTATUS ReadMemory(void *source, void *target, ULONGLONG size)
{
	ULONG transferred;

	if(!targetProcess) return STATUS_INVALID_PARAMETER_1;

	return MmCopyVirtualMemoryRoutine(targetProcess,source,currentProcess,target,size,KernelMode,&transferred);

}

/*
This is a wrapper around KeDelayExecutionThread. Since KeyDelayExecutionThread takes an input of 100ns units, we convert 
our ms input, and use a negative value for relative time.
*/

NTSTATUS Sleep(ULONGLONG milliseconds)
{
	LARGE_INTEGER delay;
	ULONG *split;

	milliseconds*=1000000;

	milliseconds/=100;

	milliseconds=-milliseconds;

	split=(ULONG*)&milliseconds;

	delay.LowPart=*split;

	split++;

	delay.HighPart=*split;

	KeDelayExecutionThread(KernelMode,0,&delay);

	return STATUS_SUCCESS;
}

//Image name is also in this structure, but we use msdn provided info because it is less likely to change.
struct SYSTEM_PROCESS_INFORMATION
{
	ULONG NextEntryOffset;
	char Reserved1[52];
	PVOID Reserved2[3];
	HANDLE UniqueProcessId;
	PVOID Reserved3;
	ULONG HandleCount;
	char Reserved4[4];
	PVOID Reserved5[11];
	SIZE_T PeakPagefileUsage;
	SIZE_T PrivatePageCount;
	LARGE_INTEGER Reserved6[6];
};

//we could walk vads but since ldr_data's appearance on msdn it is less likely to change. 

/*
This is a very nasty function to retrieve the linear address of the specified module. It walks the PEB loader
lists of the target process and resolves the specified module name to a linear address. 
*/
NTSTATUS GetModuleBase(wchar_t *ModuleName, ULONGLONG *base)
{
	ULONGLONG ldr;
	ULONGLONG pdata=0;
	ULONGLONG buffer=0;
	ULONGLONG head=0;
	ULONGLONG string=0;
	wchar_t dllname[16];
	int i=0;

	*base=0;

	if(!targetProcess) return STATUS_INVALID_PARAMETER_1;

	ldr=(ULONGLONG)PsGetPeb32(targetProcess);

	if(!ldr)
	{
		ldr=PsGetPeb64(targetProcess);

		ldr+=0x18;

		/**********************************************************/


		if(ReadMemory((void*)ldr,&pdata,8)) return STATUS_INVALID_PARAMETER_1;

		pdata+=0x10;

		head=pdata;

		while(i<500)
		{
			if(ReadMemory((void*)pdata,&buffer,8)) return STATUS_INVALID_PARAMETER_1;

			if(buffer==head) return 1;

			buffer+=0x60;

			if(ReadMemory((void*)buffer,&string,8)) return STATUS_INVALID_PARAMETER_1;

			if(ReadMemory((void*)string,dllname,sizeof(dllname))) return STATUS_INVALID_PARAMETER_1;

			if(!wcscmp(ModuleName,dllname))
			{
				buffer-=0x30;

				if(ReadMemory((void*)buffer,&pdata,8)) return STATUS_INVALID_PARAMETER_1;

				*base=pdata;

				return STATUS_SUCCESS;
			}

			i++;

			buffer-=0x60;

			pdata=buffer;


		}



		return STATUS_INVALID_PARAMETER_1;

		/**********************************************************/

	}

	ldr+=0xc;

	if(ReadMemory((void*)ldr,&pdata,4)) return STATUS_INVALID_PARAMETER_1;

	pdata+=0xc;

	head=pdata;

	while(i<500)
	{
		if(ReadMemory((void*)pdata,&buffer,4)) return STATUS_INVALID_PARAMETER_1;

		if(buffer==head) return 1;

		buffer+=0x30;

		if(ReadMemory((void*)buffer,&string,4)) return STATUS_INVALID_PARAMETER_1;

		if(ReadMemory((void*)string,dllname,sizeof(dllname))) return STATUS_INVALID_PARAMETER_1;

		if(!wcscmp(ModuleName,dllname))
		{
			buffer-=0x18;

			if(ReadMemory((void*)buffer,&pdata,4)) return STATUS_INVALID_PARAMETER_1;

			*base=pdata;

			return STATUS_SUCCESS;
		}

		i++;

		buffer-=0x30;

		pdata=buffer;


	}


	return STATUS_INVALID_PARAMETER_1;
}

/*
This function uses ZwQuerySystemInformation and PsLookupProcessByProcessId to resolve the EPROCESS for the specified 
image name.
*/

NTSTATUS AttachToProcess(char *ImageName)
{

	ULONG entryOffset;
	NTSTATUS status;
	ULONG lenActual;
	struct SYSTEM_PROCESS_INFORMATION *sysinfo;
	char *sys;
	char *zero;
	PEPROCESS pbuffer;
	ANSI_STRING filename;
	char found=0;

	currentProcess=PsGetCurrentProcess();

	if(targetProcess)
	{
		ObDereferenceObject(targetProcess);

		targetProcess=0;
	}


	sys=(char*)ExAllocatePoolWithTag(PagedPool,0x50000,NULL);

	zero=sys;

	sysinfo=(struct SYSTEM_PROCESS_INFORMATION*)sys;

	status=ZwQuerySystemInformation(0x5,(void*)sys,0x50000,&lenActual);

	if(status)
	{
		ExFreePool((PVOID)sys);

		return STATUS_INVALID_PARAMETER_1;
	}

	while(1) //lul
	{

		if(!PsLookupProcessByProcessId(sysinfo->UniqueProcessId,&pbuffer))
		{

			if(!strcmp(ImageName,PsGetImageName(pbuffer)))
			{
				targetProcess=pbuffer;

				found=1;

				break;
			}

			ObDereferenceObject(pbuffer);


		}

		if(!sysinfo->NextEntryOffset) break;

		sys+=sysinfo->NextEntryOffset;

		sysinfo=(struct SYSTEM_PROCESS_INFORMATION*)sys;

	}

	ExFreePool((PVOID)zero);

	if(!found) return STATUS_INVALID_PARAMETER_1;

	return STATUS_SUCCESS;
}


/*
These are the hooks for win32k!InputApc which filter mouse and keyboard. ReadInstrumentation modifies the 
completion KAPC by hooking mouclass and kbdclass MJ_READ routines. This is how HID/8042 input is monitored. Yes 
this is nasty, but afaik only i8042 provides functions for filtering. The user could have 8042 or USB, or both.
*/

NTSTATUS KeyboardApc(void *a1, void *a2, void *a3, void *a4, void *a5)
{
	unsigned char max=(unsigned char)mjRead->MakeCode;


	if(!mjRead->Flags)
	{
		KEY_DATA[(max)-1]=1;
	}
	else if(mjRead->Flags&KEY_BREAK)
	{
		KEY_DATA[(max)-1]=0;
	}

	return KeyboardInputRoutine(a1,a2,a3,a4,a5);
}

NTSTATUS MouseApc(void *a1, void *a2, void *a3, void *a4, void *a5)
{
	if(mouIrp->ButtonFlags&MOUSE_LEFT_BUTTON_DOWN)
	{
		MOU_DATA[0]=1;
	}
	else if(mouIrp->ButtonFlags&MOUSE_LEFT_BUTTON_UP)
	{
		MOU_DATA[0]=0;
	}
	else if(mouIrp->ButtonFlags&MOUSE_RIGHT_BUTTON_DOWN)
	{
		MOU_DATA[1]=1;
	}
	else if(mouIrp->ButtonFlags&MOUSE_RIGHT_BUTTON_UP)
	{
		MOU_DATA[1]=0;
	}
	else if(mouIrp->ButtonFlags&MOUSE_MIDDLE_BUTTON_DOWN)
	{
		MOU_DATA[2]=1;
	}
	else if(mouIrp->ButtonFlags&MOUSE_MIDDLE_BUTTON_UP)
	{
		MOU_DATA[2]=0;
	}
	else if(mouIrp->ButtonFlags&MOUSE_BUTTON_4_DOWN)
	{
		MOU_DATA[3]=1;
	}
	else if(mouIrp->ButtonFlags&MOUSE_BUTTON_4_UP)
	{
		MOU_DATA[3]=0;
	}
	else if(mouIrp->ButtonFlags&MOUSE_BUTTON_5_DOWN)
	{
		MOU_DATA[4]=1;
	}
	else if(mouIrp->ButtonFlags&MOUSE_BUTTON_5_UP)
	{
		MOU_DATA[4]=0;
	}

	return MouseInputRoutine(a1,a2,a3,a4,a5);
}

/*
These routines hijack the IRP's kapc for keyboard and mouse input respectively.

*/

NTSTATUS ReadInstrumentation(PDEVICE_OBJECT device, PIRP irp)
{
	ULONGLONG *routine;

	routine=(ULONGLONG*)irp;

	routine+=0xb;


	if(!KeyboardInputRoutine)
	{
		KeyboardInputRoutine=(kbdinput)*routine;
	}

	*routine=(ULONGLONG)KeyboardApc;

	mjRead=(struct KEYBOARD_INPUT_DATA*)irp->UserBuffer;

	return KbdClassReadRoutine(device,irp);
}

NTSTATUS ReadInstrumentation1(PDEVICE_OBJECT device, PIRP irp)
{
	ULONGLONG *routine;

	routine=(ULONGLONG*)irp;

	routine+=0xb;


	if(!MouseInputRoutine)
	{
		MouseInputRoutine=(mouinput)*routine;
	}

	*routine=(ULONGLONG)MouseApc;

	mouIrp=(struct KEYBOARD_INPUT_DATA*)irp->UserBuffer;

	return MouClassReadRoutine(device,irp);
}

NTSTATUS Edox_InvalidRequest(PDEVICE_OBJECT device, PIRP irp);


/*
This routine serves to respond to the connect request from kbdclass/mouclass after an
adddevice call. The corresponding DPC routines are then saved in the global pointers described above.
*/

NTSTATUS Edox_InternalIoctl(PDEVICE_OBJECT device, PIRP irp)
{
	PIO_STACK_LOCATION ios;
	PCONNECT_DATA cd;

	ios=IoGetCurrentIrpStackLocation(irp);

	if(ios->Parameters.DeviceIoControl.IoControlCode==MOUCLASS_CONNECT_REQUEST)
	{
		cd=ios->Parameters.DeviceIoControl.Type3InputBuffer;

		MouseDpcRoutine=(MouseServiceDpc)cd->ClassService;
	}
	else if(ios->Parameters.DeviceIoControl.IoControlCode==KBDCLASS_CONNECT_REQUEST)
	{
		cd=ios->Parameters.DeviceIoControl.Type3InputBuffer;

		KeyboardDpcRoutine=(KeyboardServiceDpc)cd->ClassService;
	}
	else
	{
		Edox_InvalidRequest(device,irp);
	}

	return STATUS_SUCCESS;
}

NTSTATUS Edox_InvalidRequest(PDEVICE_OBJECT device, PIRP irp)
{
	return STATUS_SUCCESS;	
}


/*
This function recursively searches for a devnode to hijack in a device stack, this is so we can properly call 
mouclass/kbdclass adddevice routines and pretend we are an actual HID provider. This is just a giant hack and our driver should 
really be part of the USB stack.
*/

void *FindDevNodeRecurse(PDEVICE_OBJECT a1, ULONGLONG *a2)
{
	struct DEVOBJ_EXTENSION_FIX *attachment;

	attachment=a1->DeviceObjectExtension;

	if((!attachment->AttachedTo)&&(!attachment->DeviceNode)) return;

	if((!attachment->DeviceNode)&&(attachment->AttachedTo))
	{
		FindDevNodeRecurse(attachment->AttachedTo,a2);

		return;
	}

	*a2=(ULONGLONG)attachment->DeviceNode;

	return;
}

/*==============================================================================*/

ULONG filter(void* a1)
{
	return 0;
}

NTSTATUS DriverEntry(IN PDRIVER_OBJECT driverObject, IN PUNICODE_STRING regPath)
{
	UNICODE_STRING symbolicLink;
	UNICODE_STRING deviceName;
	PDEVICE_OBJECT devicePtr;
	int i=0;
	PDEVICE_OBJECT input_mouse;
	PDEVICE_OBJECT input_keyboard;
	CLIENT_ID nthread;
	PKTHREAD p=NULL;
	PEXCEPTION_POINTERS ptr;


	/*==============================*/
	UNICODE_STRING classNameBuffer;
	UNICODE_STRING routineName;
	PDEVICE_OBJECT classObj;
	PFILE_OBJECT file;
	PDRIVER_OBJECT classDrv;
	MouseAddDevice MouseAddDevicePtr;
	KeyboardAddDevice KeyboardAddDevicePtr;
	struct DEVOBJ_EXTENSION_FIX *DevObjExtension;
	ULONGLONG node=0;
	SHORT *u;  
	USHORT charBuff;
	wchar_t kbdname[23]=L"\\Device\\KeyboardClass0";
	wchar_t mouname[22]=L"\\Device\\PointerClass0";
	void *linear=(void*)ReadInstrumentation;
	HANDLE thread;
	/*==============================*/

	memset((void*)&mdata,0,sizeof(mdata));
	memset((void*)&kdata,0,sizeof(kdata));
	memset((void*)MOU_DATA,0,sizeof(MOU_DATA));

	RtlInitUnicodeString(&routineName,L"MmCopyVirtualMemory");

	MmCopyVirtualMemoryRoutine=(MmCopyVirtualMemory)MmGetSystemRoutineAddress(&routineName);

	RtlInitUnicodeString(&routineName,L"ZwQuerySystemInformation");

	ZwQuerySystemInformation=(NtQuerySystemInformation)MmGetSystemRoutineAddress(&routineName);

	RtlInitUnicodeString(&routineName,L"PsGetProcessImageFileName");

	PsGetImageName=(PsGetProcessImageFileName)MmGetSystemRoutineAddress(&routineName);

	RtlInitUnicodeString(&routineName,L"PsGetProcessPeb");

	PsGetPeb64=(PsGetProcessPeb)MmGetSystemRoutineAddress(&routineName);

	RtlInitUnicodeString(&routineName,L"PsGetProcessWow64Process");

	PsGetPeb32=(PsGetProcessWow64Process)MmGetSystemRoutineAddress(&routineName);

	/**/
	//RtlInitUnicodeString(&deviceName,L"\\Device\\edoxHID");

	IoCreateDevice(driverObject,0,NULL,FILE_DEVICE_UNKNOWN,FILE_DEVICE_SECURE_OPEN,FALSE,&devicePtr);

	//RtlInitUnicodeString(&symbolicLink,L"\\DosDevices\\mkInput");

	//IoCreateSymbolicLink(&symbolicLink,&deviceName);

	/**/

	RtlInitUnicodeString(&deviceName,L"\\Device\\edoxMouse");

	IoCreateDevice(driverObject,0,&deviceName,FILE_DEVICE_UNKNOWN,FILE_DEVICE_SECURE_OPEN,FALSE,&input_mouse);

	RtlInitUnicodeString(&deviceName,L"\\Device\\edoxKeyboard");

	IoCreateDevice(driverObject,0,&deviceName,FILE_DEVICE_UNKNOWN,FILE_DEVICE_SECURE_OPEN,FALSE,&input_keyboard);


	for(i=0; i<IRP_MJ_MAXIMUM_FUNCTION; i++) 
		driverObject->MajorFunction[i]=Edox_InvalidRequest;


	driverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL]=Edox_InternalIoctl;
	driverObject->MajorFunction[IRP_MJ_READ]=Edox_InvalidRequest;
	driverObject->MajorFunction[IRP_MJ_CREATE]=Edox_InvalidRequest;
	driverObject->MajorFunction[IRP_MJ_CLOSE]=Edox_InvalidRequest;
	driverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]=Edox_InvalidRequest;
	driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]=Edox_InvalidRequest;
	driverObject->MajorFunction[IRP_MJ_CLEANUP]=Edox_InvalidRequest;
	driverObject->MajorFunction[IRP_MJ_POWER]=Edox_InvalidRequest;
	driverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL]=Edox_InvalidRequest;
	driverObject->MajorFunction[IRP_MJ_PNP]=Edox_InvalidRequest;


	devicePtr->Flags|=DO_BUFFERED_IO; 
	devicePtr->Flags&=~DO_DEVICE_INITIALIZING;


	input_mouse->Flags|=DO_BUFFERED_IO; 
	input_mouse->Flags&=~DO_DEVICE_INITIALIZING;

	input_keyboard->Flags|=DO_BUFFERED_IO; 
	input_keyboard->Flags&=~DO_DEVICE_INITIALIZING;

	/*==============================*/

	//find a devnode we can hijack

	RtlInitUnicodeString(&classNameBuffer,mouname);

	u=mouname;


	while(1)
	{
		//run till we run out of devices or find a devnode

		if(IoGetDeviceObjectPointer(&classNameBuffer,FILE_ALL_ACCESS,&file,&classObj)) return STATUS_OBJECT_NAME_NOT_FOUND;

		ObDereferenceObject(file);

		node=FindDevNodeRecurse(classObj,&node);

		if(node) break;

		*(u+MOU_STRING_INC)+=1;

		mouId++;

	}

	mouTarget=classObj;

	classDrv=classObj->DriverObject;

	MouClassReadRoutine=(MouclassRead)classDrv->MajorFunction[IRP_MJ_READ];

	classDrv->MajorFunction[IRP_MJ_READ]=ReadInstrumentation1;

	DevObjExtension=input_mouse->DeviceObjectExtension;

	DevObjExtension->DeviceNode=(void*)node;

	MouseAddDevicePtr=(MouseAddDevice)classDrv->DriverExtension->AddDevice;

	MouseAddDevicePtr(classDrv,input_mouse);

	//repeat same process for keyboard stacks

	RtlInitUnicodeString(&classNameBuffer,kbdname);

	u=kbdname;


	charBuff=*(u+KBD_STRING_INC);


	while(1)
	{
		//run till we run out of devices or find a devnode

		if(IoGetDeviceObjectPointer(&classNameBuffer,FILE_ALL_ACCESS,&file,&classObj)) return STATUS_OBJECT_NAME_NOT_FOUND;

		ObDereferenceObject(file);

		node=FindDevNodeRecurse(classObj,&node);

		if(node) break;

		*(u+KBD_STRING_INC)+=1;

		kbdId++;

	}


	*(u+KBD_STRING_INC)=charBuff;

	kbdTarget=classObj;

	classDrv=classObj->DriverObject;

	DevObjExtension=input_keyboard->DeviceObjectExtension;

	DevObjExtension->DeviceNode=(void*)node;

	KeyboardAddDevicePtr=(KeyboardAddDevice)classDrv->DriverExtension->AddDevice;

	KeyboardAddDevicePtr(classDrv,input_keyboard);

	/**/
	KbdClassReadRoutine=(KbdclassRead)classDrv->MajorFunction[IRP_MJ_READ];

	classDrv->MajorFunction[IRP_MJ_READ]=ReadInstrumentation;

	for(i=0; i<128; i++) KEY_DATA[i]=0;

	PsCreateSystemThread(&thread,STANDARD_RIGHTS_ALL,NULL,NULL,&nthread,(PKSTART_ROUTINE)SystemRoutine,NULL);


	ZwClose(thread);



	
	return STATUS_SUCCESS;
} 

