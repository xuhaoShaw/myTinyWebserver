# myTinyWebserver
2021.12.01 - 2021.12.10 参考游双《Linux高性能服务器编程》，自学web服务器开发项目

## 效果
### 开发环境

> - Ubuntu 18.04
> - gcc 7.5

### 编译

```c++
xh@xh:~/Linux/webserver$ g++ *.cpp -pthread
```

### 访问方式

- 在终端运行程序：./a.out 10000
- 输入 IP:端口号，如192.168.226.136:10000


## 压力测试

### 测试方式

在压力测试终端运行webbench程序：

```c++
./webbench -c 1000 -t 5 http://192.168.226.136:10000/index.html
```

其中，-c表示同时建立起多少个连接，-t表示连接的访问时间（单位s）

### 测试结果
![image-20220320161909269](https://user-images.githubusercontent.com/43106882/169474002-c4ed4d50-bf96-43d9-8e28-06cd3be9b4b0.png)

### 可完善之处

- 现在用proactor模式，可改为reactor模式；
- 现在用LT，可改为ET；
- 现在只支持get，可增加post功能；
- 定时器断开长时间没有响应的连接。
