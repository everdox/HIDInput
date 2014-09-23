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
#include "input.h"


//////////////////////////////////
//                               //
//        FUNCTION LIST          //
//                               //
///////////////////////////////////                             

/*
Sleep(int milliseconds) - Just like usermode, takes this thread off the processor for the specified duration.

AttachToProcess(char *imageName) - Attaches to the specified process. The image name is just the binary, not a fully qualified image path.

GetModuleBase(wchar_t *moduleName, ULONGLONG *base) - obtains the linear base address for the specified module name. You must have previously and
successfully called AttachToProcess() or this function will fail.

ReadMemory(void *source, void *target, ULONGLONG size) - Speaks for itself I hope. You must have previously and successfully called AttachToProcess() 
or this function will fail. 

SynthesizeMouse(PMOUSE_INPUT_DATA a1) - Synthesizes the corresponding mouse input.

SynthesizeKeyboard(PMOUSE_INPUT_DATA a1) - Synthesizes the corresponding keyboard input.

GetKeyState(char scan) - Asynchronously retrieves the up or down state of the specified can code.

GetMouseState(int key) - Asynchronously retrieves the up or down state of the specified mouse button.

0 - Left mouse
1 - Right mouse
2 - Middle button
3 - Mouse button 4
4 - Mouse button 5


*/



NTSTATUS SystemRoutine()
{
	//DO YOUR WORK HERE:

	return STATUS_SUCCESS;
}
