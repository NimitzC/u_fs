# 基于FUSE3.3的文件系统
## 作者
16计科1班 曾昭彧 201630598565

## 源文件
```bash
u_fs.c         #u_fs文件系统源代码
diskimg_init.c #用于初始化磁盘文件diskimg
```
## 注意事项
详细的过程可以在课程设计报告的"**四、结果分析**"找到
+ u_fs.c中写死了虚拟磁盘文件diskimg的位置，如想更改路径请修改源码中的全局变量`DISKIMG_PATH`
+ 目录名不能带后缀(8.0 format)
+ mknod不能在根目录下建文件
+ 编译，打开终端执行`make`命令

## 测试
新建一个虚拟磁盘文件
```bash
$ dd bs=1K count=5K if=/dev/zero of=diskimg
```

初始化虚拟磁盘文件
```bash
$ ./diskimg_init diskimg
```

挂载文件系统
```bash
$ mkdir testmount
$ ./u_fs -d testmount #你可能需要修改u_fs.c中DISKIMG_PATH为实际值才能正常运行
```

打开一个新的终端进行测试
```bash
$ cd testmount
$ ls -al
```


## 支持的命令
```bash
$ ls -al
$ mkdir
$ rmdir
$ echo
$ cat
$ unlink
```