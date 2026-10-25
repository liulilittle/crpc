# CRPC 服务定义体验设计（工作流 ②）

日期：2026-10-25
状态：设计已批准，待写实现计划

## 背景

当前 RPC 服务定义是低级 API：

```cpp
builder.add_service(service_id(1), "svc");
builder.add_method<MyCtl, &MyCtl::get_user>(service_id(1), method_id(1), "user.get", ctl);
```

需要手写 service_id/method_id，逐个注册。MVC 侧已有 `CRPC_HTTP_GET` 等声明式宏，RPC 侧缺失对应体验。

## 目标

- 声明式宏 `CRPC_RPC_METHOD(Ctl, Method, "name")` 声明 RPC 方法。
- 自动生成 service_id/method_id（从类型名 + 方法名确定性哈希）。
- `server::builder::add_controller<Ctl>(instance)` 自动注册 `Ctl` 中所有声明方法。
- 参数绑定：RTTI 属性 `CRPC_FROM_ROUTE/QUERY/BODY/HEADER` 自动绑定请求字段到方法参数；序列化对用户透明。
- 保留现有 `add_method<Ctl, Method>` 低级 API。

## 设计

### 1. `CRPC_RPC_METHOD` 宏

```cpp
#define CRPC_RPC_METHOD(Ctl, Method, name_literal)                              \
    inline static constexpr ::crpc::rpc::method_meta crpc_rpc_meta_##Method {   \
        ::crpc::rpc::method_id_from<Ctl>(), name_literal};                      \
    friend class ::crpc::rpc::service_registrar<Ctl>;
```

- `method_id_from<Ctl>()` 返回确定性哈希（FNV-1a of type name）。
- `method_meta` 携带 service_id、方法名。

### 2. `add_controller<Ctl>(instance)`

```cpp
template <typename Ctl>
builder& add_controller(Ctl& instance) {
    rpc::register_controller<Ctl>(registrations_, instance);
    return *this;
}
```

`register_controller` 用编译期遍历 `Ctl` 的 `crpc_rpc_meta_*` 静态成员，逐个生成 `add_method` 注册项。

### 3. 参数绑定

- 请求结构体字段用 `CRPC_FROM_BODY` / `CRPC_FROM_ROUTE` / `CRPC_FROM_QUERY` / `CRPC_FROM_HEADER` 标注。
- 服务端 `register_typed_method` 已用 RTTI 反射绑定；新增宏只生成注册，不重复绑定逻辑。

### 4. 兼容性

- 保留 `add_service` / `add_method` / `add_raw_method`。
- 新宏与旧 API 可混用。

## 测试

- 声明式注册：`add_controller<Ctl>` 后方法可调用。
- 自动 ID：两个不同 Ctl/Method 的 service_id/method_id 不同。
- 参数绑定：`CRPC_FROM_BODY` 请求字段自动绑定。
- 零手写序列化：客户端 `client.request<Resp>(...)` 与服务端声明式方法互通。

## 迁移

- 旧代码不改。
- 新代码用 `CRPC_RPC_METHOD` + `add_controller`。

## ABI

- 新增宏与模板，不改现有类型布局。
- 跑 `abi_check`。
