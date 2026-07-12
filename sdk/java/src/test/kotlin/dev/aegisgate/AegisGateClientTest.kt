package dev.aegisgate

import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.jupiter.api.AfterEach
import org.junit.jupiter.api.BeforeEach
import org.junit.jupiter.api.Test
import org.junit.jupiter.api.assertThrows
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

class AegisGateClientTest {

    private lateinit var server: MockWebServer
    private lateinit var client: AegisGateClient

    @BeforeEach
    fun setUp() {
        server = MockWebServer()
        server.start()
        client = AegisGateClient(
            AegisGateConfig(
                apiKey = "test-key",
                baseUrl = server.url("/").toString(),
                timeoutMs = 5000,
                connectTimeoutMs = 2000,
                maxRetries = 2,
                retryDelayMs = 100,
                retryJitter = false,
            )
        )
    }

    @AfterEach
    fun tearDown() {
        client.close()
        server.shutdown()
    }

    @Test
    fun testChatCompletions() {
        val responseBody = """
        {
            "id": "chatcmpl-123",
            "object": "chat.completion",
            "model": "gpt-4o",
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": "Hello! How can I help you?"
                    },
                    "finish_reason": "stop"
                }
            ],
            "usage": {
                "prompt_tokens": 10,
                "completion_tokens": 8,
                "total_tokens": 18
            }
        }
        """.trimIndent()

        server.enqueue(MockResponse().setBody(responseBody).setHeader("Content-Type", "application/json"))

        val response = client.chatCompletions(
            ChatCompletionRequest(
                model = "gpt-4o",
                messages = listOf(ChatMessage(role = "user", content = "Hello")),
                temperature = 0.7,
                maxTokens = 1000,
            )
        )

        assertEquals("chatcmpl-123", response.id)
        assertEquals("gpt-4o", response.model)
        assertEquals(1, response.choices.size)
        assertEquals("Hello! How can I help you?", response.choices[0].message.content)
        assertEquals("assistant", response.choices[0].message.role)
        assertEquals("stop", response.choices[0].finishReason)
        assertNotNull(response.usage)
        assertEquals(10, response.usage!!.promptTokens)
        assertEquals(8, response.usage!!.completionTokens)
        assertEquals(18, response.usage!!.totalTokens)
        assertEquals("Hello! How can I help you?", response.content)

        val recorded = server.takeRequest()
        assertEquals("POST", recorded.method)
        assertEquals("/v1/chat/completions", recorded.path)
        assertTrue(recorded.getHeader("Authorization")!!.contains("test-key"))
    }

    @Test
    fun testChatCompletionsStream() = runBlocking {
        val sseBody = """
data: {"id":"chatcmpl-1","object":"chat.completion.chunk","model":"gpt-4o","choices":[{"index":0,"delta":{"role":"assistant","content":"Hello"},"finish_reason":null}]}

data: {"id":"chatcmpl-1","object":"chat.completion.chunk","model":"gpt-4o","choices":[{"index":0,"delta":{"content":" world"},"finish_reason":null}]}

data: {"id":"chatcmpl-1","object":"chat.completion.chunk","model":"gpt-4o","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}

data: [DONE]

        """.trimIndent()

        server.enqueue(
            MockResponse()
                .setBody(sseBody)
                .setHeader("Content-Type", "text/event-stream")
        )

        val chunks = client.chatCompletionsStream(
            ChatCompletionRequest(
                model = "gpt-4o",
                messages = listOf(ChatMessage(role = "user", content = "Hello")),
            )
        ).toList()

        assertTrue(chunks.isNotEmpty())
        assertEquals("chatcmpl-1", chunks[0].id)
        assertEquals("Hello", chunks[0].choices[0].delta.content)
        assertEquals(" world", chunks[1].choices[0].delta.content)
        assertEquals("stop", chunks[2].choices[0].finishReason)
    }

    @Test
    fun testListModels() {
        val responseBody = """
        {
            "object": "list",
            "data": [
                {"id": "gpt-4o", "object": "model", "owned_by": "openai"},
                {"id": "gpt-3.5-turbo", "object": "model", "owned_by": "openai"}
            ]
        }
        """.trimIndent()

        server.enqueue(MockResponse().setBody(responseBody).setHeader("Content-Type", "application/json"))

        val models = client.listModels()

        assertEquals("list", models.objectType)
        assertEquals(2, models.data.size)
        assertEquals("gpt-4o", models.data[0].id)
        assertEquals("openai", models.data[0].ownedBy)
        assertEquals("gpt-3.5-turbo", models.data[1].id)

        val recorded = server.takeRequest()
        assertEquals("GET", recorded.method)
        assertEquals("/v1/models", recorded.path)
    }

    @Test
    fun testHealth() {
        val responseBody = """{"status": "ok", "version": "1.0.0"}"""

        server.enqueue(MockResponse().setBody(responseBody).setHeader("Content-Type", "application/json"))

        val health = client.health()

        assertEquals("ok", health.status)
        assertEquals("1.0.0", health.version)

        val recorded = server.takeRequest()
        assertEquals("GET", recorded.method)
        assertEquals("/health", recorded.path)
        assertTrue(recorded.getHeader("Authorization") == null || recorded.getHeader("Authorization")!!.isEmpty())
    }

    @Test
    fun testRetryOn429() {
        server.enqueue(
            MockResponse()
                .setResponseCode(429)
                .setBody("""{"message": "rate limited"}""")
                .setHeader("Content-Type", "application/json")
        )

        val successBody = """
        {
            "id": "chatcmpl-retry",
            "object": "chat.completion",
            "model": "gpt-4o",
            "choices": [{"index": 0, "message": {"role": "assistant", "content": "Success after retry"}, "finish_reason": "stop"}]
        }
        """.trimIndent()

        server.enqueue(MockResponse().setBody(successBody).setHeader("Content-Type", "application/json"))

        val response = client.chatCompletions(
            ChatCompletionRequest(
                model = "gpt-4o",
                messages = listOf(ChatMessage(role = "user", content = "Hello")),
            )
        )

        assertEquals("Success after retry", response.content)
        assertEquals(2, server.requestCount)
    }

    @Test
    fun testAuthenticationError() {
        server.enqueue(
            MockResponse()
                .setResponseCode(401)
                .setBody("""{"message": "Invalid API key"}""")
                .setHeader("Content-Type", "application/json")
        )

        val exception = assertThrows<AuthenticationException> {
            client.chatCompletions(
                ChatCompletionRequest(
                    model = "gpt-4o",
                    messages = listOf(ChatMessage(role = "user", content = "Hello")),
                )
            )
        }

        assertEquals("Invalid API key", exception.message)
        assertEquals(401, exception.statusCode)
        assertEquals(1, server.requestCount)
    }

    @Test
    fun testRetryExhausted() {
        repeat(3) {
            server.enqueue(
                MockResponse()
                    .setResponseCode(500)
                    .setBody("""{"message": "Internal server error"}""")
                    .setHeader("Content-Type", "application/json")
            )
        }

        val exception = assertThrows<ApiException> {
            client.chatCompletions(
                ChatCompletionRequest(
                    model = "gpt-4o",
                    messages = listOf(ChatMessage(role = "user", content = "Hello")),
                )
            )
        }

        assertEquals(500, exception.statusCode)
        assertEquals(3, server.requestCount)
    }

    @Test
    fun testMetrics() {
        val metricsText = """
            # HELP http_requests_total Total HTTP requests
            # TYPE http_requests_total counter
            http_requests_total{method="GET"} 100
        """.trimIndent()

        server.enqueue(MockResponse().setBody(metricsText).setHeader("Content-Type", "text/plain"))

        val result = client.metrics()

        assertTrue(result.contains("http_requests_total"))
    }

    @Test
    fun testTraceHeaders() {
        server.enqueue(MockResponse().setBody("""{"status": "ok", "version": "1.0.0"}"""))

        val tracedClient = AegisGateClient(
            AegisGateConfig(
                apiKey = "test-key",
                baseUrl = server.url("/").toString(),
                traceId = "trace-123",
                traceHeaders = mapOf("traceparent" to "00-abc-def-01"),
                defaultHeaders = mapOf("X-Custom" to "custom-value"),
            )
        )

        tracedClient.use {
            it.health()
        }

        val recorded = server.takeRequest()
        assertEquals("trace-123", recorded.getHeader("X-Trace-Id"))
        assertEquals("00-abc-def-01", recorded.getHeader("traceparent"))
        assertEquals("custom-value", recorded.getHeader("X-Custom"))
    }

    @Test
    fun testChatConvenience() {
        val responseBody = """
        {
            "id": "chatcmpl-conv",
            "object": "chat.completion",
            "model": "gpt-4o",
            "choices": [{"index": 0, "message": {"role": "assistant", "content": "I am an assistant"}, "finish_reason": "stop"}]
        }
        """.trimIndent()

        server.enqueue(MockResponse().setBody(responseBody).setHeader("Content-Type", "application/json"))

        val result = client.chat("Who are you?", model = "gpt-4o", system = "You are helpful.")

        assertEquals("I am an assistant", result)

        val recorded = server.takeRequest()
        val body = recorded.body.readUtf8()
        assertTrue(body.contains("\"system\""))
        assertTrue(body.contains("You are helpful."))
        assertTrue(body.contains("Who are you?"))
    }
}
