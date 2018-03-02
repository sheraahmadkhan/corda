@file:JvmName("AMQPSerializationScheme")

package net.corda.nodeapi.internal.serialization.amqp

import net.corda.core.cordapp.Cordapp
import net.corda.core.serialization.*
import net.corda.nodeapi.internal.serialization.CordaSerializationMagic
import net.corda.core.utilities.ByteSequence
import net.corda.nodeapi.internal.serialization.DefaultWhitelist
import net.corda.nodeapi.internal.serialization.MutableClassWhitelist
import net.corda.nodeapi.internal.serialization.SerializationScheme
import net.corda.nodeapi.internal.serialization.amqp.custom.*
import java.security.PublicKey
import java.util.*
import java.util.concurrent.ConcurrentHashMap

val AMQP_ENABLED get() = SerializationDefaults.P2P_CONTEXT.preferredSerializationVersion == amqpMagic

fun SerializerFactory.addToWhitelist(vararg types: Class<*>) {
    require(types.toSet().size == types.size) {
        val duplicates = types.toMutableList()
        types.toSet().forEach { duplicates -= it }
        "Cannot add duplicate classes to the whitelist ($duplicates)."
    }
    for (type in types) {
        (this.whitelist as? MutableClassWhitelist)?.add(type)
    }
}

abstract class AbstractAMQPSerializationScheme(val cordappLoader: List<Cordapp>) : SerializationScheme {
    companion object {
        private val serializationWhitelists: List<SerializationWhitelist> by lazy {
            ServiceLoader.load(SerializationWhitelist::class.java, this::class.java.classLoader).toList() + DefaultWhitelist
        }

        fun registerJDKTypeSerializers(factory: SerializerFactory) {
            with(factory) {
                register(ThrowableSerializer(this))
                register(PrivateKeySerializer)
                register(BigDecimalSerializer)
                register(CurrencySerializer)
                register(InstantSerializer(this))
                register(DurationSerializer(this))
                register(LocalDateSerializer(this))
                register(LocalDateTimeSerializer(this))
                register(LocalTimeSerializer(this))
                register(ZonedDateTimeSerializer(this))
                register(ZoneIdSerializer(this))
                register(OffsetTimeSerializer(this))
                register(OffsetDateTimeSerializer(this))
                register(YearSerializer(this))
                register(YearMonthSerializer(this))
                register(MonthDaySerializer(this))
                register(PeriodSerializer(this))
                register(ClassSerializer(this))
                register(X509CertificateSerializer)
                register(CertPathSerializer(this))
                register(StringBufferSerializer)
                register(SimpleStringSerializer)
                register(InputStreamSerializer)
                register(BitSetSerializer(this))
                register(EnumSetSerializer(this))
            }
        }
    }

    private fun registerCustomSerializers(factory: SerializerFactory) {
        registerJDKTypeSerializers(factory)
        factory.register(publicKeySerializer)
        factory.register(OpaqueBytesSubSequenceSerializer(factory))
        factory.register(ContractAttachmentSerializer(factory))
        for (whitelistProvider in serializationWhitelists) {
            factory.addToWhitelist(*whitelistProvider.whitelist.toTypedArray())
        }
        for (loader in cordappLoader) {
            for (schema in loader.serializationCustomSerializers) {
                factory.registerExternal(CorDappCustomSerializer(schema, factory))
            }
        }
    }

    private val serializerFactoriesForContexts = ConcurrentHashMap<Pair<ClassWhitelist, ClassLoader>, SerializerFactory>()

    protected abstract fun rpcClientSerializerFactory(context: SerializationContext): SerializerFactory
    protected abstract fun rpcServerSerializerFactory(context: SerializationContext): SerializerFactory
    protected open val publicKeySerializer: CustomSerializer.Implements<PublicKey>
            = net.corda.nodeapi.internal.serialization.amqp.custom.PublicKeySerializer

    private fun getSerializerFactory(context: SerializationContext): SerializerFactory {
        return serializerFactoriesForContexts.computeIfAbsent(Pair(context.whitelist, context.deserializationClassLoader)) {
            when (context.useCase) {
                SerializationContext.UseCase.Checkpoint ->
                    throw IllegalStateException("AMQP should not be used for checkpoint serialization.")
                SerializationContext.UseCase.RPCClient ->
                    rpcClientSerializerFactory(context)
                SerializationContext.UseCase.RPCServer ->
                    rpcServerSerializerFactory(context)
                else -> SerializerFactory(context.whitelist, context.deserializationClassLoader)
            }
        }.also { registerCustomSerializers(it) }
    }

    override fun <T : Any> deserialize(byteSequence: ByteSequence, clazz: Class<T>, context: SerializationContext): T {
        val serializerFactory = getSerializerFactory(context)
        return DeserializationInput(serializerFactory).deserialize(byteSequence, clazz)
    }

    override fun <T : Any> serialize(obj: T, context: SerializationContext): SerializedBytes<T> {
        val serializerFactory = getSerializerFactory(context)
        return SerializationOutput(serializerFactory).serialize(obj)
    }

    protected fun canDeserializeVersion(magic: CordaSerializationMagic) = magic == amqpMagic
}

// TODO: This will eventually cover server RPC as well and move to node module, but for now this is not implemented
class AMQPServerSerializationScheme(cordapps: List<Cordapp> = emptyList()) : AbstractAMQPSerializationScheme(cordapps) {
    override fun rpcClientSerializerFactory(context: SerializationContext): SerializerFactory {
        throw UnsupportedOperationException()
    }

    override fun rpcServerSerializerFactory(context: SerializationContext): SerializerFactory {
        TODO("not implemented") //To change body of created functions use File | Settings | File Templates.
    }

    override fun canDeserializeVersion(magic: CordaSerializationMagic, target: SerializationContext.UseCase): Boolean {
        return canDeserializeVersion(magic) &&
                (target == SerializationContext.UseCase.P2P || target == SerializationContext.UseCase.Storage)
    }

}

// TODO: This will eventually cover client RPC as well and move to client module, but for now this is not implemented
class AMQPClientSerializationScheme(cordapps: List<Cordapp> = emptyList()) : AbstractAMQPSerializationScheme(cordapps) {
    override fun rpcClientSerializerFactory(context: SerializationContext): SerializerFactory {
        TODO("not implemented") //To change body of created functions use File | Settings | File Templates.
    }

    override fun rpcServerSerializerFactory(context: SerializationContext): SerializerFactory {
        throw UnsupportedOperationException()
    }

    override fun canDeserializeVersion(magic: CordaSerializationMagic, target: SerializationContext.UseCase): Boolean {
        return canDeserializeVersion(magic) &&
                (target == SerializationContext.UseCase.P2P || target == SerializationContext.UseCase.Storage)
    }

}

