package dev.aegisgate

import com.google.gson.Gson
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.sse.EventSource
import okhttp3.sse.EventSourceListener
import okhttp3.sse.EventSources
import java.io.Closeable
import java.io.IOException
import java.net.SocketTimeoutException
import java.util.concurrent.TimeUnit
import kotlin.math.pow
import kotlin.random.Random

private val RETRYABLE_STATUS_CODES = setOf(429, 500, 502, 503, 504)
private val JSON_MEDIA_TYPE = "application/json; charset=utf-8".toMediaType()

class AegisGateClient(
    private val config: AegisGateConfig,
) : Closeable {

    private val gson = Gson()
    private val baseUrl = config.baseUrl.trimEnd('/')

    private val httpClient: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(config.connectTimeoutMs, TimeUnit.MILLISECONDS)
        .readTimeout(config.timeoutMs, TimeUnit.MILLISECONDS)
        .writeTimeout(config.timeoutMs, TimeUnit.MILLISECONDS)
        .build()

    private fun buildRequest(
        path: String,
        method: String = "GET",
        body: String? = null,
        auth: Boolean = true,
    ): Request {
        val builder = Request.Builder()
            .url("$baseUrl$path")

        if (auth) {
            builder.addHeader("Authorization", "Bearer ${config.apiKey}")
        }
        builder.addHeader("Content-Type", "application/json")

        for ((k, v) in config.defaultHeaders) {
            builder.addHeader(k, v)
        }
        for ((k, v) in config.traceHeaders) {
            builder.addHeader(k, v)
        }
        config.traceId?.let { traceId ->
            if (config.traceHeaders.none { it.key.equals("X-Trace-Id", ignoreCase = true) }) {
                builder.addHeader("X-Trace-Id", traceId)
            }
        }

        when (method.uppercase()) {
            "POST" -> builder.post((body ?: "").toRequestBody(JSON_MEDIA_TYPE))
            "GET" -> builder.get()
            else -> builder.method(method, body?.toRequestBody(JSON_MEDIA_TYPE))
        }
        return builder.build()
    }

    private fun parseRetryAfter(response: Response): Double? {
        val header = response.header("Retry-After") ?: return null
        return header.toDoubleOrNull()
    }

    private fun backoffDelay(attempt: Int): Long {
        val delay = config.retryDelayMs * 2.0.pow(attempt)
        return if (config.retryJitter) {
            (delay * (0.5 + Random.nextDouble() * 0.5)).toLong()
        } else {
            delay.toLong()
        }
    }

    private fun handleErrorResponse(response: Response): Nothing {
        val body = response.body?.string() ?: ""
        val statusCode = response.code

        var message = "API error: $statusCode"
        var errorCode: String? = null
        try {
            val json = JsonParser.parseString(body).asJsonObject
            message = json.get("message")?.asString
                ?: json.get("error")?.let {
                    if (it.isJsonObject) it.asJsonObject.get("message")?.asString
                    else it.asString
                }
                ?: message
            errorCode = json.get("code")?.asString
                ?: json.get("error_code")?.asString
        } catch (_: Exception) {
            if (body.isNotBlank()) message = body
        }

        when (statusCode) {
            401 -> throw AuthenticationException(message, responseBody = body)
            429 -> throw RateLimitException(
                message,
                responseBody = body,
                retryAfterSeconds = response.header("Retry-After")?.toDoubleOrNull(),
            )
            else -> throw ApiException(
                message,
                statusCode = statusCode,
                errorCode = errorCode,
                responseBody = body,
            )
        }
    }

    private fun <T> executeWithRetry(action: () -> T): T {
        var lastException: Exception? = null
        for (attempt in 0..config.maxRetries) {
            try {
                return action()
            } catch (e: AuthenticationException) {
                throw e
            } catch (e: ApiException) {
                lastException = e
                if (e.statusCode in RETRYABLE_STATUS_CODES && attempt < config.maxRetries) {
                    Thread.sleep(backoffDelay(attempt))
                } else {
                    throw e
                }
            } catch (e: SocketTimeoutException) {
                lastException = e
                if (attempt < config.maxRetries) {
                    Thread.sleep(backoffDelay(attempt))
                } else {
                    throw TimeoutException("Request timed out", e)
                }
            } catch (e: IOException) {
                lastException = e
                if (attempt < config.maxRetries) {
                    Thread.sleep(backoffDelay(attempt))
                } else {
                    throw ConnectionException("Connection failed: ${e.message}", e)
                }
            }
        }
        throw lastException ?: ConnectionException("Max retries exceeded")
    }

    fun chatCompletions(request: ChatCompletionRequest): ChatCompletionResponse {
        val req = request.copy(stream = false)
        val body = gson.toJson(req)
        return executeWithRetry {
            val httpRequest = buildRequest("/v1/chat/completions", "POST", body)
            val response = httpClient.newCall(httpRequest).execute()
            response.use {
                if (!it.isSuccessful) handleErrorResponse(it)
                val responseBody = it.body?.string()
                    ?: throw AegisGateException("Empty response body")
                gson.fromJson(responseBody, ChatCompletionResponse::class.java)
            }
        }
    }

    fun chatCompletionsStream(request: ChatCompletionRequest): Flow<ChatCompletionChunk> {
        val req = request.copy(stream = true)
        val body = gson.toJson(req)
        val httpRequest = buildRequest("/v1/chat/completions", "POST", body)

        return callbackFlow {
            val factory = EventSources.createFactory(httpClient)
            val listener = object : EventSourceListener() {
                override fun onEvent(eventSource: EventSource, id: String?, type: String?, data: String) {
                    if (data == "[DONE]") {
                        close()
                        return
                    }
                    try {
                        val chunk = gson.fromJson(data, ChatCompletionChunk::class.java)
                        trySend(chunk)
                    } catch (_: Exception) {
                        // skip unparseable events
                    }
                }

                override fun onFailure(eventSource: EventSource, t: Throwable?, response: Response?) {
                    if (response != null && !response.isSuccessful) {
                        val responseBody = try { response.body?.string() ?: "" } catch (_: Exception) { "" }
                        val statusCode = response.code
                        val exception = when (statusCode) {
                            401 -> AuthenticationException(responseBody = responseBody)
                            429 -> RateLimitException(responseBody = responseBody)
                            else -> ApiException(
                                "Stream error: $statusCode",
                                statusCode = statusCode,
                                responseBody = responseBody,
                            )
                        }
                        close(exception)
                    } else if (t != null) {
                        val exception = when (t) {
                            is SocketTimeoutException -> TimeoutException("Stream timed out", t)
                            is IOException -> ConnectionException("Stream connection failed: ${t.message}", t)
                            else -> AegisGateException("Stream error: ${t.message}", t)
                        }
                        close(exception)
                    } else {
                        close()
                    }
                }

                override fun onClosed(eventSource: EventSource) {
                    close()
                }
            }

            val eventSource = factory.newEventSource(httpRequest, listener)

            awaitClose {
                eventSource.cancel()
            }
        }
    }

    @JvmOverloads
    fun chat(
        content: String,
        model: String = "gpt-4o",
        system: String? = null,
        temperature: Double = 0.7,
        maxTokens: Int = 1000,
    ): String {
        val messages = mutableListOf<ChatMessage>()
        system?.let { messages.add(ChatMessage(role = "system", content = it)) }
        messages.add(ChatMessage(role = "user", content = content))

        val response = chatCompletions(
            ChatCompletionRequest(
                model = model,
                messages = messages,
                temperature = temperature,
                maxTokens = maxTokens,
            )
        )
        return response.content
    }

    fun listModels(): ModelListResponse {
        return executeWithRetry {
            val request = buildRequest("/v1/models")
            val response = httpClient.newCall(request).execute()
            response.use {
                if (!it.isSuccessful) handleErrorResponse(it)
                val body = it.body?.string()
                    ?: throw AegisGateException("Empty response body")
                gson.fromJson(body, ModelListResponse::class.java)
            }
        }
    }

    fun health(): HealthResponse {
        val request = buildRequest("/health", auth = false)
        val response = httpClient.newCall(request).execute()
        return response.use {
            if (!it.isSuccessful) handleErrorResponse(it)
            val body = it.body?.string()
                ?: throw AegisGateException("Empty response body")
            gson.fromJson(body, HealthResponse::class.java)
        }
    }

    fun metrics(): String {
        val request = buildRequest("/metrics")
        val response = httpClient.newCall(request).execute()
        return response.use {
            if (!it.isSuccessful) handleErrorResponse(it)
            it.body?.string() ?: ""
        }
    }

    override fun close() {
        httpClient.dispatcher.executorService.shutdown()
        httpClient.connectionPool.evictAll()
    }
}
