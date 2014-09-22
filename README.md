HIDInput
========

HIDInput was developed with the idea of synthesizing mouse and keyboard input from a system thread, as well as supplementing the task in the system thread with easy-to-use functions that made it feel like the end-coder was working in user-mode. Some examples are: ReadMemory(), SynthesizeMouse(), SynthesizeKeyboard(), AttachToProcess(), GetModuleBase(), and get key/mouse state functions in an asynchronous manner.

In the end, the idea was to have a fully kernel based framework in which mouse and keyboard input can be synthesized based off of data probes to the attached process. 

In this way, the end-coder does not require a high knowledge of kernel driver development, and can use the easy to call functions just as if he or she was doing the same project, but targeted for a user-mode environment.
