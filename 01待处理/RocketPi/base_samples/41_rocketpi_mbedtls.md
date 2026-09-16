---
status: done
created: 2026-09-16
tags:
  - c/crypto
  - c/mbedtls
  - embedded/security
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/41_rocketpi_mbedtls"
  - "[[40_rocketpi_crc]] CRC 校验（对比 mbedTLS 哈希）"
---

# 41 mbedTLS 加密算法

## 一句话定性

mbedTLS 是开源加密库，在 STM32F401 上实现 AES-128 对称加密、RSA-1024 非对称加密、SHA-256 哈希三种密码学原语，是嵌入式系统数据安全的基础（固件签名、安全通信、数据加密等）。

## 同类产品定位

- **mbedTLS**：开源加密库，MIT 许可，适合嵌入式，代码量 ~100KB
- **wolfSSL**：商业加密库，更小更快
- **OpenSSL**：服务端加密库，太大不适合嵌入式
- **硬件加密**：STM32F4 无内置加密外设（F4 有 AES 硬件加速，F401 没有）

## 硬件连接

- 无外部器件，纯软件加密
- UART2 用于输出测试结果

## 密码学基础

### 对称加密（AES）

- **AES-128**：128 位密钥，16 字节块加密
- **ECB 模式**：最简单，每块独立加密（不推荐用于实际应用，应加 IV 用 CBC/CTR）
- **用途**：数据加密（如 EEPROM 配置数据加密存储）

### 非对称加密（RSA）

- **RSA-1024**：1024 位密钥对，公钥加密+私钥解密
- **PKCS#1 v1.5**：填充方案，保证安全性
- **用途**：固件签名验证、密钥交换、数字证书

### 哈希（SHA-256）

- **SHA-256**：256 位哈希值，单向不可逆
- **用途**：数据完整性校验、密码存储、数字签名

## 代码架构

```
main.c (13KB，全部逻辑)
├── run_aes_test：AES-128 ECB 加解密验证（NIST 向量）
├── run_rsa_test：RSA-1024 PKCS#1 v1.5 加解密（固定密钥对）
├── run_sha256_test：SHA-256 哈希验证（固定输入+预期输出）
└── print_hex / log_status：调试输出辅助
```

## 核心实现

### run_aes_test——AES-128 ECB 加解密

```c
static int run_aes_test(void)
{
    // NIST 标准测试向量
    const uint8_t key[16] = {0x2b, 0x7e, 0x15, 0x16, ...};
    const uint8_t plain[16] = {0x6b, 0xc1, 0xbe, 0xe2, ...};
    const uint8_t expected_cipher[16] = {0x3a, 0xd7, 0x7b, 0xb4, ...};

    // ① 加密
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, plain, cipher);

    // ② 验证密文
    if (memcmp(cipher, expected_cipher, 16) != 0) return -1;

    // ③ 解密
    mbedtls_aes_setkey_dec(&aes, key, 128);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, cipher, decrypted);

    // ④ 验证明文
    if (memcmp(decrypted, plain, 16) != 0) return -1;

    mbedtls_aes_free(&aes);
    return 0;
}
```

- **NIST 向量**：使用 NIST（美国国家标准技术研究院）发布的标准测试数据，确保实现正确性
- **ECB 模式**：每 16 字节独立加密，相同明文→相同密文。**不推荐用于实际应用**（模式泄露），应改用 CBC 或 CTR
- **`mbedtls_aes_free`**：释放 AES 上下文资源，防止内存泄漏

### run_rsa_test——RSA-1024 加解密

```c
static int run_rsa_test(void)
{
    // ⑤ 导入固定密钥对（十六进制字符串）
    mbedtls_mpi_read_string(&value, 16, RSA_N_HEX);  // 模数 N
    mbedtls_rsa_import(&rsa, &value, NULL, NULL, NULL, NULL);
    // ... 导入 P, Q, D, E ...
    mbedtls_rsa_complete(&rsa);  // 补全密钥参数

    // ⑥ 验证密钥
    mbedtls_rsa_check_pubkey(&rsa);
    mbedtls_rsa_check_privkey(&rsa);

    // ⑦ 加密
    mbedtls_rsa_pkcs1_encrypt(&rsa, pseudo_entropy_source, &seed,
                              MBEDTLS_RSA_PUBLIC, sizeof(message)-1, message, ciphertext);

    // ⑧ 解密
    mbedtls_rsa_pkcs1_decrypt(&rsa, pseudo_entropy_source, &seed,
                              MBEDTLS_RSA_PRIVATE, &olen, ciphertext, decrypted, sizeof(decrypted));

    // ⑨ 验证
    if (memcmp(decrypted, message, olen) != 0) return -1;
}
```

- **⑤ 固定密钥对**：N/E/D/P/Q 五个参数以十六进制字符串硬编码。实际应用中私钥应安全存储（如安全芯片、加密 Flash）
- **⑥ 密钥验证**：`check_pubkey` 验证公钥数学性质，`check_privkey` 验证私钥与公钥匹配
- **⑦⑧ PKCS#1 v1.5**：填充方案，将明文填充到密钥长度（128 字节），防止选择密文攻击
- **⑨ `pseudo_entropy_source`**：伪随机数生成器（LCG 算法），用于 PKCS#1 填充。**实际应用必须用真随机源**（硬件 RNG）

### run_sha256_test——SHA-256 哈希

```c
static int run_sha256_test(void)
{
    const unsigned char message[] = "RocketPi MBEDTLS";
    const uint8_t expected_hash[32] = {0xb9, 0x2f, 0x7c, ...};

    // ⑩ 初始化 → 更新 → 完成
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts_ret(&ctx, 0);      // 0 = SHA-256（非 SHA-224）
    mbedtls_sha256_update_ret(&ctx, message, sizeof(message)-1);
    mbedtls_sha256_finish_ret(&ctx, hash);

    // ⑪ 验证
    if (memcmp(hash, expected_hash, 32) != 0) return -1;

    mbedtls_sha256_free(&ctx);
    return 0;
}
```

- **⑩ 三步式 API**：`starts` → `update`（可多次）→ `finish`。支持流式处理（大文件分块哈希）
- **⑪ 256 位哈希**：32 字节，无论输入多长，输出固定 32 字节

### pseudo_entropy_source——伪随机数

```c
static int pseudo_entropy_source(void *ctx, unsigned char *output, size_t len)
{
    uint32_t *seed = (uint32_t *)ctx;
    for (size_t i = 0; i < len; i++) {
        *seed = (*seed * 1664525UL) + 1013904223UL;  // LCG 算法
        output[i] = (uint8_t)(*seed >> 24);
    }
    return 0;
}
```

**LCG（线性同余生成器）**：最简单的伪随机算法，周期 2^32。**不安全**——可预测，仅用于测试。实际应用必须用硬件 RNG（STM32F4 有 RNG 外设，但 F401 没有）。

## 设计问题与改进空间

1. **ECB 模式不安全**：相同明文块产生相同密文块，模式泄露。实际应用应改用 CBC（需 IV）或 CTR（需 nonce）。

2. **RSA-1024 密钥太短**：NIST 已废弃 RSA-1024，建议至少 RSA-2048。但 2048 位密钥在 F401 上加解密可能需要几秒。

3. **伪随机数不安全**：LCG 算法可预测。实际应用必须用真随机源（硬件 RNG 或外部噪声源）。

4. **固定密钥对**：测试用固定密钥，实际应用中私钥应安全存储（安全芯片、加密 Flash、密钥派生）。

5. **mbedTLS 代码量**：~100KB Flash + ~20KB RAM，对 F401RE（512KB Flash/96KB RAM）来说可接受，但需裁剪不需要的算法。

6. **与 40 例（CRC）对比**：CRC 是校验算法（检测意外错误），mbedTLS 是密码学（防恶意篡改）。CRC 不防攻击，SHA-256 防攻击。

## 关联笔记

- [[40_rocketpi_crc|40 CRC 校验]]：数据完整性校验，对比 SHA-256 哈希
- [[37_rocketpi_esp8266|37 ESP8266 WiFi]]：MQTT over TLS 需要 mbedTLS 支持
