package dev.aegisgate

open class AegisGateException(
    message: String,
    cause: Throwable? = null,
) : RuntimeException(message, cause)

open class ApiException(
    message: String,
    val statusCode: Int,
    val errorCode: String? = null,
    val responseBody: String? = null,
    cause: Throwable? = null,
) : AegisGateException(message, cause)

class AuthenticationException(
    message: String = "Authentication failed",
    responseBody: String? = null,
) : ApiException(message, statusCode = 401, responseBody = responseBody)

class RateLimitException(
    message: String = "Rate limit exceeded",
    responseBody: String? = null,
    val retryAfterSeconds: Double? = null,
) : ApiException(message, statusCode = 429, responseBody = responseBody)

class TimeoutException(
    message: String = "Request timed out",
    cause: Throwable? = null,
) : AegisGateException(message, cause)

class ConnectionException(
    message: String = "Connection failed",
    cause: Throwable? = null,
) : AegisGateException(message, cause)
