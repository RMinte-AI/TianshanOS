# 生产删除/规则提交的 HTTP 字节夹具

由 tests/runtime/test_delete_reference.py 提取当前生产处理函数、规则提交和模板删除边界，并经过当前生产 HTTP 响应分支生成。SSH/持久化/文件删除边界是合成替身；不访问设备。夹具不是 api.call 的伪造对象。

正常回归比较实际字节，不改夹具。只有合同经过审查改变时，用 UPDATE_FIXTURES=1 python3 tests/runtime/test_delete_reference.py 更新。

in-use：包含停用、重复引用去重和默认容量32条规则；in-use-no-details：引用已确认但细节分配失败；check-loading/uninitialized/recovery：配置状态；action-missing：新引用被拒绝且未保存；boundary：名称47字节和ID63字节；service-protected：后端最终服务保护。浏览器测试直接读取这些文件原样作为 HTTP 响应。
