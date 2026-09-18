#!/usr/bin/env python3
"""End-to-end verification for every bundled obfuscation fixture.

The script deliberately uses only Python's standard library so GitHub Actions
and local contributors execute the same test matrix without extra tooling.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path
from typing import Optional


ROOT = Path(__file__).resolve().parents[1]
TEST_ROOT = ROOT / "test"
PROJECTS = TEST_ROOT / "projects"
CONFIGS = TEST_ROOT / "configs"
WORK = TEST_ROOT / ".work"
EXPECTED_CLI = "CLI_OK:314214:hello-codex:dynamic-resource-ok:SUPER_SECRET_MESSAGE"


def run(command: list[str], cwd: Path, timeout: int = 120) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command))
    result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode != 0:
        raise AssertionError("command failed:\n" + result.stdout)
    return result


def require_output(result: subprocess.CompletedProcess[str], expected: str) -> None:
    if expected not in result.stdout:
        raise AssertionError("missing output %r:\n%s" % (expected, result.stdout))


def build(project: str, *properties: str) -> None:
    # A fixture may have been built with another JDK in a previous run. Always
    # remove stale class files so this matrix validates the selected runtime.
    run(["mvn", "-B", "-q", "-DskipTests", "clean", "package", *properties], PROJECTS / project)


def obfuscate(obfuscator: Path, source: Path, config_name: str) -> Path:
    case = WORK / config_name.removesuffix(".yaml")
    case.mkdir(parents=True, exist_ok=True)
    input_jar = case / "input" / source.name
    input_jar.parent.mkdir(exist_ok=True)
    shutil.copy2(source, input_jar)
    config = CONFIGS / config_name
    run(["java", "-jar", str(obfuscator), "--jar", str(input_jar), "--config", str(config)], case)
    # Runner always emits a JAR archive, including when the input is a WAR.
    output = case / (source.stem + "_obf.jar")
    if not output.is_file():
        raise AssertionError("obfuscator did not create " + str(output))
    if (case / "jar-obf-lib").exists():
        raise AssertionError("obsolete jar-obf-lib directory was created")
    return output


def zip_bytes(path: Path) -> bytes:
    with zipfile.ZipFile(path) as archive:
        return b"".join(archive.read(name) for name in archive.namelist() if name.endswith(".class"))


def assert_missing(path: Path, text: str) -> None:
    if text.encode("utf-8") in zip_bytes(path):
        raise AssertionError("unobfuscated marker remains: " + text)


def run_cli(path: Path) -> None:
    result = run(["java", "-Xverify:all", "-jar", str(path)], path.parent)
    require_output(result, EXPECTED_CLI)


def test_cli(obfuscator: Path) -> None:
    source = PROJECTS / "cli-app" / "target" / "cli-app-1.0.0.jar"
    profiles = [
        "cli-class.yaml", "cli-package.yaml", "cli-method.yaml", "cli-field.yaml",
        "cli-param.yaml", "cli-xor.yaml", "cli-string.yaml", "cli-advanced-string.yaml",
        "cli-metadata.yaml", "cli-junk-1.yaml", "cli-junk-2.yaml", "cli-junk-3.yaml",
        "cli-junk-4.yaml", "cli-junk-5.yaml", "cli-all.yaml",
    ]
    outputs: dict[str, Path] = {}
    for profile in profiles:
        output = obfuscate(obfuscator, source, profile)
        run_cli(output)
        outputs[profile] = output

    with zipfile.ZipFile(outputs["cli-class.yaml"]) as archive:
        if "fixtures/cli/Worker.class" in archive.namelist():
            raise AssertionError("class-name obfuscation did not rename Worker")
    with zipfile.ZipFile(outputs["cli-package.yaml"]) as archive:
        if "fixtures/cli/Worker.class" in archive.namelist():
            raise AssertionError("package-name obfuscation did not move Worker")
    method_listing = run(["javap", "-p", "-classpath", str(outputs["cli-method.yaml"]),
                          "fixtures.cli.Worker"], outputs["cli-method.yaml"].parent)
    if "calculateInternal(" in method_listing.stdout:
        raise AssertionError("method-name obfuscation did not rename calculateInternal")
    field_listing = run(["javap", "-p", "-classpath", str(outputs["cli-field.yaml"]),
                         "fixtures.cli.Worker"], outputs["cli-field.yaml"].parent)
    if "secretValue;" in field_listing.stdout:
        raise AssertionError("field-name obfuscation did not rename secretValue")
    parameter_listing = run(["javap", "-v", "-p", "-classpath", str(outputs["cli-param.yaml"]),
                             "fixtures.cli.Worker"], outputs["cli-param.yaml"].parent)
    local_variable_section = parameter_listing.stdout.split("MethodParameters:", 1)[0]
    if " calculateInput " in local_variable_section:
        raise AssertionError("parameter-name obfuscation did not rename local variable metadata")
    for profile in ("cli-string.yaml", "cli-advanced-string.yaml", "cli-all.yaml"):
        string_listing = run(["javap", "-c", "-classpath", str(outputs[profile]), "fixtures.cli.Main"],
                             outputs[profile].parent)
        if "SUPER_SECRET_MESSAGE" in string_listing.stdout:
            raise AssertionError("string encryption left plaintext instructions in " + profile)

    metadata = run(["javap", "-v", "-p", "-classpath", str(outputs["cli-metadata.yaml"]),
                    "fixtures.cli.Worker"], outputs["cli-metadata.yaml"].parent)
    require_output(metadata, "ACC_SYNTHETIC")
    if "SourceFile:" in metadata.stdout or "LineNumberTable:" in metadata.stdout:
        raise AssertionError("compile-information deletion did not remove debug attributes")
    junk_five = run(["javap", "-c", "-classpath", str(outputs["cli-junk-5.yaml"]),
                      "fixtures.cli.Main"], outputs["cli-junk-5.yaml"].parent)
    require_output(junk_five, "nanoTime")


def test_library(obfuscator: Path) -> None:
    source = PROJECTS / "library-app" / "target" / "library-app-1.0.0.jar"
    # Consumers are compiled against the original public API, as in a real
    # published-library upgrade. The obfuscated JAR must remain link-compatible.
    build("library-consumer")
    output = obfuscate(obfuscator, source, "library.yaml")
    consumer = PROJECTS / "library-consumer" / "target" / "library-consumer-1.0.0.jar"
    result = run(["java", "-Xverify:all", "-cp", str(consumer) + ":" + str(output),
                  "fixtures.consumer.Main"], consumer.parent)
    require_output(result, "LIBRARY_OK:library-consumer-24")


def test_swing(obfuscator: Path) -> None:
    source = PROJECTS / "swing-app" / "target" / "swing-app-1.0.0.jar"
    output = obfuscate(obfuscator, source, "swing.yaml")
    result = run(["java", "-Xverify:all", "-Djava.awt.headless=true", "-jar", str(output)], output.parent)
    require_output(result, "SWING_OK:after")


def test_spring_boot(obfuscator: Path) -> None:
    source = PROJECTS / "spring-boot-app" / "target" / "spring-boot-app-1.0.0.jar"
    output = obfuscate(obfuscator, source, "spring-boot.yaml")
    log = output.parent / "spring.log"
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(["java", "-Xverify:all", "-jar", str(output)], cwd=output.parent,
                                   stdout=stream, stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.monotonic() + 45
        last_error: Optional[Exception] = None
        while time.monotonic() < deadline:
            try:
                with urllib.request.urlopen("http://127.0.0.1:18080/health", timeout=2) as response:
                    if response.read().decode("utf-8") == "SPRING_OK":
                        return
            except Exception as exc:  # Server startup is expected to race polling.
                last_error = exc
                time.sleep(0.5)
        raise AssertionError("Spring Boot endpoint did not become healthy: %s\n%s" %
                             (last_error, log.read_text(encoding="utf-8")))
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


def test_web_war(obfuscator: Path) -> None:
    source = PROJECTS / "web-war" / "target" / "web-war-1.0.0.war"
    output = obfuscate(obfuscator, source, "web-war.yaml")
    extracted = output.parent / "war-content"
    with zipfile.ZipFile(output) as archive:
        archive.extractall(extracted)
    classes = extracted / "WEB-INF" / "classes"
    result = run(["java", "-Xverify:all", "-cp", str(classes), "fixtures.war.WarEntry"], extracted)
    require_output(result, "WAR_OK:war-42")
    web_xml = (extracted / "WEB-INF" / "web.xml").read_text(encoding="utf-8")
    if "fixtures.war.WarEntry" not in web_xml:
        raise AssertionError("WAR resource transformation corrupted web.xml")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--obfuscator", type=Path, required=True)
    args = parser.parse_args()
    obfuscator = args.obfuscator.resolve()
    if not obfuscator.is_file():
        parser.error("obfuscator JAR does not exist: " + str(obfuscator))

    shutil.rmtree(WORK, ignore_errors=True)
    WORK.mkdir(parents=True)
    build("cli-app")
    build("library-app")
    build("swing-app")
    build("web-war")
    build("spring-boot-app")
    test_cli(obfuscator)
    test_library(obfuscator)
    test_swing(obfuscator)
    test_spring_boot(obfuscator)
    test_web_war(obfuscator)
    print("ALL_OBFUSCATION_TESTS_PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, subprocess.TimeoutExpired) as error:
        print("TEST_FAILURE:", error, file=sys.stderr)
        sys.exit(1)
