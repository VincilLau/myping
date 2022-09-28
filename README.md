# myping

使用 `Linux 原始套接字`实现的 `ping` 命令。

## 构建

```bash
git clone https://github.com/vincillau/myping.git
cd myping
xmake f -m release
xmake build
```

## 运行

```bash
sudo ./build/linux/x86_64/release/myping 127.0.0.1
```

## 注意

- 暂时不支持 IPV6
- 暂时不支持解析域名

## 维护者

[@Vincil Lau](https://github.com/vincillau)

## 许可证

[MIT](./LICENSE)
