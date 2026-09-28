项目用于对安装后的p4v客户端进行patch，增加一些官方没有的功能。

p4v安装后的路径目录：C:\Apps\Perforce


# auto relogin

p4v登录过期后会弹出“Perforce Password Required”的弹窗，内容是“A password is required for user 'xxxx' on server 'xxxxx'.”

本patch希望能在此窗口中增加一个“Save”的按钮，如果点击了Save则将密码记录到内存中，再下次将要弹出或者弹出后，自动输入记忆的密码并执行重新登录，如果自动登录失败3次，则停止自动重复登录，恢复原始弹出的显示，等待用户自己处理。
