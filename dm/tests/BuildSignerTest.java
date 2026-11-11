package com.nxnative;

import java.nio.charset.StandardCharsets;

public final class BuildSignerTest {
    private static void check(boolean ok, String what) { if (!ok) throw new AssertionError(what); }

    public static void main(String[] args) {
        // the FIPS 180-2 "abc" vector, lowercase hex as apksigner prints digests
        check(BuildSigner.sha256("abc".getBytes(StandardCharsets.US_ASCII)).equals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 hex");
        check(BuildSigner.RELEASE_CERT.matches("[0-9a-f]{64}"), "release digest is 64 lowercase hex digits");

        String other = "891ccf74aa90db816f18fbc446a6fba9a44acc8b3fc8dc9838b2f2f9fe55d556";   // the pre-v0.37 debug key: not the release key
        check(BuildSigner.official(new String[] { BuildSigner.RELEASE_CERT }), "release key is official");
        check(BuildSigner.official(new String[] { BuildSigner.RELEASE_CERT.toUpperCase() }), "digest case does not matter");
        check(!BuildSigner.official(new String[] { other }), "another key is unofficial");
        check(!BuildSigner.official(new String[] { BuildSigner.RELEASE_CERT, other }), "an extra signer is unofficial");
        check(!BuildSigner.official(new String[0]), "no signers is unofficial");
        check(!BuildSigner.official((String[]) null), "unknown signers is unofficial");

        check(BuildSigner.label(new String[] { BuildSigner.RELEASE_CERT }).equals("official"), "official label");
        check(BuildSigner.label(new String[] { other }).equals("unofficial " + other), "unofficial label names the key");
        check(BuildSigner.label(new String[0]).equals("unofficial unknown"), "unknown label");
        System.out.println("BuildSignerTest: ok");
    }
}
