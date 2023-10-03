## Chat-Meow-ESP32

|  ESP-IDF   | v4.4.5  |
|  ----  | ----  |
| ESP-ADF  | ESP-ADF Master |
| ESP32 Board  | ESP32-LyraT-Mini or MeowBoard (ESP32 WROVER)|


### 环境安装
使用的是 ESP-IDF v4.4 理论上支持ESP-IDF v5

- 参考 [ESP-IDF 编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/get-started/index.html) 安装ESP-DF环境
- 参考 [乐鑫音频应用开发指南](https://docs.espressif.com/projects/esp-adf/zh_CN/latest/get-started/index.html) 安装ESP-ADF环境(如果已配置好VSCode+ESP-IDF插件环境，在vscode`命令面板 -> ESP-IDF:安装ESP-ADF` 可以直接安装ESP-ADF)

### 配置参数

需要先配置 Wi-Fi 连接信息，通过运行 `menuconfig > Example Configuration` 填写 `Wi-Fi SSID` 和 `Wi-Fi Password`。

```
menuconfig > Example Configuration > (myssid) WiFi SSID > (myssid) WiFi Password
```

其次需要选择服务器地址, ，默认情况下`192.168.8.5:8000`需要修改为自己的服务器地址
```
menuconfig > Example Configuration > Server URL to send data > Server FILE URL to play voice
```

### 编译和下载

请先编译版本并烧录到开发板上，然后运行 monitor 工具来查看串口输出（替换 PORT 为端口名称）：

```
idf.py build
idf.py -p PORT flash monitor
```

### 开始

- 开始运行后，将主动连接 Wi-Fi 热点
- 检测是否按下REC(ESP32-LyraT-Mini)按键,如果使用喵板则需要修改按键参数
- 从麦克风读取语音上传到服务器
- 从服务器播放服务器生成好的语言
- 循环-->>>>

### 即将支持
- 静默状态识别自动识别是否讲话，读取语音开始循环
- 通过`ChatGPT函数调用`功能返回参数，接口对外输出信息，控制其他实体设备


