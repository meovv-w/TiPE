// Standard Win32 EDIT control exercised through Wine's IME/XIM bridge.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
static HWND editor;
static int sensitive;
static void snapshot(void);
static void caret_snapshot(HWND window) {
  if(GetFileAttributesW(L"reset.request")!=INVALID_FILE_ATTRIBUTES) {
    SetWindowTextW(editor,L"");SetFocus(editor);snapshot();
    DeleteFileW(L"reset.request");
  }
  GUITHREADINFO info={0};info.cbSize=sizeof(info);
  BOOL valid=GetGUIThreadInfo(GetCurrentThreadId(),&info) && info.hwndCaret==editor;
  POINT points[2]={{0,0},{0,0}};
  if(valid) {
    points[0].x=info.rcCaret.left;points[0].y=info.rcCaret.top;
    points[1].x=info.rcCaret.right;points[1].y=info.rcCaret.bottom;
    MapWindowPoints(editor,window,points,2);
  }
  POINT screen[2]={points[0],points[1]};
  if(valid) {ClientToScreen(window,&screen[0]);ClientToScreen(window,&screen[1]);}
  FILE *file=fopen("caret.json.tmp","wb");
  if(file) {
    fprintf(file,"{\"valid\":%s,\"client_rect\":[%ld,%ld,%ld,%ld],\"screen_rect\":[%ld,%ld,%ld,%ld]}\n",
            valid?"true":"false",points[0].x,points[0].y,points[1].x,points[1].y,
            screen[0].x,screen[0].y,screen[1].x,screen[1].y);
    fclose(file);MoveFileExA("caret.json.tmp","caret.json",MOVEFILE_REPLACE_EXISTING);
  }
}
static void snapshot(void) {
  if(sensitive) {
    // Password regression reports length only; never copy the field text.
    FILE *file=fopen("text.length","wb");
    if(file) {fprintf(file,"%d",GetWindowTextLengthW(editor));fclose(file);}
    return;
  }
  wchar_t wide[4096]; char utf8[16384];
  int length=GetWindowTextW(editor,wide,4096);
  int bytes=WideCharToMultiByte(CP_UTF8,0,wide,length,utf8,sizeof(utf8),NULL,NULL);
  FILE *file=fopen("text.utf8","wb");
  if(file) {fwrite(utf8,1,bytes,file);fclose(file);}
}
static LRESULT CALLBACK window_proc(HWND window,UINT msg,WPARAM w,LPARAM l) {
  if(msg==WM_CREATE) {
    DWORD style=WS_CHILD|WS_VISIBLE|WS_BORDER|
        (sensitive ? ES_PASSWORD|ES_AUTOHSCROLL : ES_MULTILINE|ES_AUTOVSCROLL);
    editor=CreateWindowW(L"EDIT",L"",style,
                         20,20,620,300,window,(HMENU)1,NULL,NULL);
    SetFocus(editor);snapshot();
    if(GetEnvironmentVariableW(L"TIPE_HARNESS_CARET",NULL,0)) SetTimer(window,1,100,NULL);
    return 0;
  }
  if(msg==WM_COMMAND && HIWORD(w)==EN_CHANGE) {snapshot();return 0;}
  if(msg==WM_SETFOCUS) {SetFocus(editor);return 0;}
  if(msg==WM_TIMER && w==1) {caret_snapshot(window);return 0;}
  if(msg==WM_DESTROY) {PostQuitMessage(0);return 0;}
  return DefWindowProcW(window,msg,w,l);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE old,LPWSTR args,int show) {
  (void)old;(void)args;
  wchar_t privacy[16];
  sensitive=GetEnvironmentVariableW(L"TIPE_HARNESS_PASSWORD",privacy,16)>0 && privacy[0]!=L'0';
  WNDCLASSW cls={0};cls.lpfnWndProc=window_proc;cls.hInstance=instance;cls.lpszClassName=L"TiPERegressionEdit";
  RegisterClassW(&cls);
  HWND window=CreateWindowW(cls.lpszClassName,L"TiPE Wine isolated regression",WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT,CW_USEDEFAULT,700,390,NULL,NULL,instance,NULL);
  ShowWindow(window,show);SetForegroundWindow(window);SetFocus(editor);
  MSG msg;while(GetMessageW(&msg,NULL,0,0)>0) {TranslateMessage(&msg);DispatchMessageW(&msg);}
  return 0;
}
