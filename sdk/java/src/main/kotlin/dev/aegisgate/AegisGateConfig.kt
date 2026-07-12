package dev.aegisgate

data class AegisGateConfig(
    val apiKey: String,
    val baseUrl: String = "http://localhost:8080",
    val timeoutMs: Long = 60_000,
    val connectTimeoutMs: Long = 10_000,
    val maxRetries: Int = 2,
    val retryDelayMs: Long = 1000,
    val retryJitter: Boolean = true,
    val traceId: String? = null,
    val traceHeaders: Map<String, String> = emptyMap(),
    val defaultHeaders: Map<String, String> = emptyMap(),
)
