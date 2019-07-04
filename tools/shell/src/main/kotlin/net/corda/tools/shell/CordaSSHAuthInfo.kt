package net.corda.tools.shell

import com.fasterxml.jackson.databind.ObjectMapper
import net.corda.core.internal.messaging.InternalCordaRPCOps
import net.corda.tools.shell.InteractiveShell.createYamlInputMapper
import net.corda.tools.shell.utlities.ANSIProgressRenderer
import org.crsh.auth.AuthInfo

class CordaSSHAuthInfo(val successful: Boolean, val rpcOps: InternalCordaRPCOps, val ansiProgressRenderer: ANSIProgressRenderer? = null, val isSsh: Boolean = false) : AuthInfo {
    override fun isSuccessful(): Boolean = successful

    val yamlInputMapper: ObjectMapper by lazy {
        createYamlInputMapper(rpcOps)
    }
}