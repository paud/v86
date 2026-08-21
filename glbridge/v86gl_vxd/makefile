# V86GL VxD Makefile - Windows 98 DDK
#
# Requires:
#   - Windows 98 DDK (or Windows 95 DDK)
#   - MASM 6.11+ (included in the DDK)
#
# Usage from a Win98 DDK build window:
#   nmake

TARGET = v86gl.vxd
OBJS   = v86gl.obj

# DDK paths - adjust if your DDK is installed elsewhere
!ifndef DDKDIR
DDKDIR = C:\98DDK
!endif

MASM   = $(DDKDIR)\bin\win98\ml.exe
LINKER = $(DDKDIR)\bin\win98\link.exe

INCDIR = /I$(DDKDIR)\inc\win98 /I$(DDKDIR)\inc

AFLAGS = /coff /c /Cx /DMASM6 /nologo $(INCDIR)
LFLAGS = /VXD /nologo /ENTRY:V86GL_Control

all: $(TARGET)

$(TARGET): $(OBJS)
	$(LINKER) $(LFLAGS) /OUT:$@ $(OBJS)

.asm.obj:
	$(MASM) $(AFLAGS) /Fo$@ $<

clean:
	-del *.obj
	-del *.vxd
	-del *.map
	-del *.sym
