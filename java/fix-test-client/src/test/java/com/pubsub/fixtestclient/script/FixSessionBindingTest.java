package com.pubsub.fixtestclient.script;

import com.pubsub.fixtestclient.fix.FixEngine;
import com.pubsub.fixtestclient.fix.LogonMode;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.verify;

class FixSessionBindingTest {

    private FixEngine fixEngine;
    private FixSessionBinding binding;

    @BeforeEach
    void setUp() {
        fixEngine = mock(FixEngine.class);
        binding = new FixSessionBinding(fixEngine);
    }

    @Test
    void logon_usesStandardModeAndDefaultTarget() throws Exception {
        binding.logon("APM001", "secret", true);

        verify(fixEngine).logon("APM001", null, "secret", true, LogonMode.STANDARD);
    }

    @Test
    void logon_withTargetOverride_usesStandardMode() throws Exception {
        binding.logon("APM001", "ALTGW", "secret", false);

        verify(fixEngine).logon("APM001", "ALTGW", "secret", false, LogonMode.STANDARD);
    }

    @Test
    void logonProprietary_usesProprietaryModeWithoutTls() throws Exception {
        binding.logonProprietary("APM001", "secret");

        verify(fixEngine).logon("APM001", null, "secret", false, LogonMode.PROPRIETARY);
    }

    @Test
    void logonProprietary_withTargetOverride_usesProprietaryModeWithoutTls() throws Exception {
        binding.logonProprietary("APM001", "ALTGW", "secret");

        verify(fixEngine).logon("APM001", "ALTGW", "secret", false, LogonMode.PROPRIETARY);
    }
}
