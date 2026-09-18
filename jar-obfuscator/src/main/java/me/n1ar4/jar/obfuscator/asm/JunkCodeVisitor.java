/*
 * MIT License
 *
 * Project URL: https://github.com/jar-analyzer/jar-obfuscator
 *
 * Copyright (c) 2024-2026 4ra1n (https://github.com/4ra1n)
 *
 * This project is distributed under the MIT license.
 *
 * https://opensource.org/license/mit
 */

package me.n1ar4.jar.obfuscator.asm;

import me.n1ar4.jar.obfuscator.Const;
import me.n1ar4.jar.obfuscator.config.BaseConfig;
import me.n1ar4.jar.obfuscator.utils.JunkUtil;
import me.n1ar4.jrandom.core.JRandom;
import me.n1ar4.log.LogManager;
import me.n1ar4.log.Logger;
import org.objectweb.asm.AnnotationVisitor;
import org.objectweb.asm.Attribute;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.FieldVisitor;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.ModuleVisitor;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.RecordComponentVisitor;
import org.objectweb.asm.TypePath;


public class JunkCodeVisitor extends ClassVisitor {
    private static final Logger logger = LogManager.getLogger();
    public static int MAX_JUNK_NUM = 1000;
    public static int JUNK_NUM = 0;
    private final BaseConfig config;
    private boolean shouldSkip;

    public JunkCodeVisitor(ClassVisitor classVisitor, BaseConfig config) {
        super(Const.ASMVersion, classVisitor);
        JUNK_NUM = 0;
        this.config = config;
        this.shouldSkip = false;
    }

    @Override
    public void visit(int version, int access, String name, String signature, String superName, String[] interfaces) {
        super.visit(version, access, name, signature, superName, interfaces);
        boolean isAbstract = (access & Opcodes.ACC_ABSTRACT) != 0;
        boolean isInterface = (access & Opcodes.ACC_INTERFACE) != 0;
        boolean isEnum = (access & Opcodes.ACC_ENUM) != 0;
        if (isAbstract || isInterface || isEnum) {
            shouldSkip = true;
        }
    }

    @Override
    public MethodVisitor visitMethod(int access, String name, String desc, String signature, String[] exceptions) {
        MethodVisitor mv = super.visitMethod(access, name, desc, signature, exceptions);
        if (shouldSkip) {
            return mv;
        } else {
            return new JunkChangerMethodAdapter(mv, this.config);
        }
    }

    @Override
    public AnnotationVisitor visitAnnotation(String descriptor, boolean visible) {
        return super.visitAnnotation(descriptor, visible);
    }

    @Override
    public AnnotationVisitor visitTypeAnnotation(int typeRef, TypePath typePath, String descriptor, boolean visible) {
        return super.visitTypeAnnotation(typeRef, typePath, descriptor, visible);
    }

    @Override
    public FieldVisitor visitField(int access, String name, String descriptor, String signature, Object value) {
        return super.visitField(access, name, descriptor, signature, value);
    }

    @Override
    public ModuleVisitor visitModule(String name, int access, String version) {
        return super.visitModule(name, access, version);
    }

    @Override
    public RecordComponentVisitor visitRecordComponent(String name, String descriptor, String signature) {
        return super.visitRecordComponent(name, descriptor, signature);
    }

    @Override
    public void visitAttribute(Attribute attribute) {
        super.visitAttribute(attribute);
    }

    @Override
    public void visitEnd() {
        if (!shouldSkip && config.getJunkLevel() >= 4 && tryConsumeJunkBudget()) {
            JunkUtil.addPrintMethod(cv);
        }
        if (!shouldSkip && config.getJunkLevel() >= 5 && tryConsumeJunkBudget()) {
            JunkUtil.addHttpCode(cv);
        }
        super.visitEnd();
    }

    private static boolean tryConsumeJunkBudget() {
        if (JUNK_NUM >= MAX_JUNK_NUM) {
            logger.debug("max junk code");
            return false;
        }
        JUNK_NUM++;
        return true;
    }

    @Override
    public void visitInnerClass(String name, String outerName, String innerName, int access) {
        super.visitInnerClass(name, outerName, innerName, access);
    }

    @Override
    public void visitNestHost(String nestHost) {
        super.visitNestHost(nestHost);
    }

    @Override
    public void visitNestMember(String nestMember) {
        super.visitNestMember(nestMember);
    }

    @Override
    public void visitOuterClass(String owner, String name, String descriptor) {
        super.visitOuterClass(owner, name, descriptor);
    }

    @Override
    public void visitPermittedSubclass(String permittedSubclass) {
        super.visitPermittedSubclass(permittedSubclass);
    }

    @Override
    public void visitSource(String source, String debug) {
        super.visitSource(source, debug);
    }

    @Override
    public ClassVisitor getDelegate() {
        return super.getDelegate();
    }

    static class JunkChangerMethodAdapter extends MethodVisitor {
        private final BaseConfig config;

        JunkChangerMethodAdapter(MethodVisitor mv, BaseConfig config) {
            super(Const.ASMVersion, mv);
            this.config = config;
        }

        @Override
        public void visitAnnotableParameterCount(int parameterCount, boolean visible) {
            super.visitAnnotableParameterCount(parameterCount, visible);
        }

        @Override
        public void visitCode() {
            super.visitCode();
            emitJunkCode();
        }

        private void emitJunkCode() {
            int level = config.getJunkLevel();
            if (level >= 1 && tryConsumeJunkBudget()) {
                emitArithmeticNoise();
            }
            if (level >= 2 && tryConsumeJunkBudget()) {
                emitStringNoise();
            }
            if (level >= 3 && tryConsumeJunkBudget()) {
                emitCollectionNoise();
            }
            if (level >= 4 && tryConsumeJunkBudget()) {
                emitStringOperationNoise();
            }
            if (level >= 5 && tryConsumeJunkBudget()) {
                emitTimeNoise();
            }
        }

        // Level 1: simple integer arithmetic with an empty entry/exit stack.
        private void emitArithmeticNoise() {
            mv.visitInsn(Opcodes.NOP);
            mv.visitInsn(Opcodes.ICONST_2);
            mv.visitInsn(Opcodes.ICONST_3);
            mv.visitInsn(Opcodes.IMUL);
            mv.visitInsn(Opcodes.POP);
        }

        // Level 2: short-lived JDK object allocation.
        private void emitStringNoise() {
            mv.visitTypeInsn(Opcodes.NEW, "java/lang/StringBuilder");
            mv.visitInsn(Opcodes.DUP);
            mv.visitLdcInsn(JRandom.getInstance().randomString(16));
            mv.visitMethodInsn(Opcodes.INVOKESPECIAL, "java/lang/StringBuilder", "<init>",
                    "(Ljava/lang/String;)V", false);
            mv.visitInsn(Opcodes.POP);
        }

        // Level 3: collection interaction with no retained local state.
        private void emitCollectionNoise() {
            mv.visitTypeInsn(Opcodes.NEW, "java/util/ArrayList");
            mv.visitInsn(Opcodes.DUP);
            mv.visitMethodInsn(Opcodes.INVOKESPECIAL, "java/util/ArrayList", "<init>", "()V", false);
            mv.visitInsn(Opcodes.DUP);
            mv.visitLdcInsn(JRandom.getInstance().randomString(12));
            mv.visitMethodInsn(Opcodes.INVOKEVIRTUAL, "java/util/ArrayList", "add",
                    "(Ljava/lang/Object;)Z", false);
            mv.visitInsn(Opcodes.POP);
            mv.visitInsn(Opcodes.POP);
        }

        // Level 4: JDK string operation that leaves the operand stack unchanged.
        private void emitStringOperationNoise() {
            mv.visitLdcInsn(JRandom.getInstance().randomString(24));
            mv.visitMethodInsn(Opcodes.INVOKEVIRTUAL, "java/lang/String", "length", "()I", false);
            mv.visitIntInsn(Opcodes.BIPUSH, 31);
            mv.visitInsn(Opcodes.IXOR);
            mv.visitInsn(Opcodes.POP);
        }

        // Level 5: category-2 values exercise a larger max-stack without branches.
        private void emitTimeNoise() {
            mv.visitMethodInsn(Opcodes.INVOKESTATIC, "java/lang/System", "nanoTime", "()J", false);
            mv.visitLdcInsn(0L);
            mv.visitInsn(Opcodes.LXOR);
            mv.visitInsn(Opcodes.POP2);
        }
    }
}
