# vresearch101: Apple implementation-defined system registers

On `--machine vresearch101` (AP and SEP) Apple's `S3_<op1>_C15_<crm>_<op2>` registers keep what the
guest writes and read it back (`HvCpu.impdef`, `core/sysreg.c`); hengevm logs each one the first
time a machine uses it (`implementation-defined ... first read/write by cpu N at pc ...`). Names
are from Asahi Linux / m1n1 and qemu-vphone where known; "?" means not confirmed.

| Register | Name | Who, where | Reset / notes |
|---|---|---|---|
| S3_6_C15_C12_4 | APSTS_EL1 | AP, iBoot's hand-off (pc 747ff5a0) | **1** (MKEYVld: the pointer-auth master key is valid). With 0 the guest polls it forever. Set in `hv_cpu_reset_index` |
| S3_6_C15_C1_0 | SPRR_CONFIG_EL1 | AP, pc 747ff4e8 | written; real behaviour is M5 |
| S3_6_C15_C1_6 | ? (SPRR) | AP, pc 747ff4f8 | written |
| S3_4_C15_C0_4 | ? | AP, read at pc 747ff5b0 | 0 |
| S3_4_C15_C1_0, S3_4_C15_C1_1 | ? | AP, pc 747ff5ec/5f4 | written |
| S3_4_C15_C10_0..3 | ? (APRR/KTRR range?) | AP pc 747ff62c..650; OpenSEP writes the same four | written |
| S3_6_C15_C1_5 | ? (SPRR) | AP, fffffe00267ff79c (before SPTM's GENTER) | written |
| S3_6_C15_C3_1 | ? (MPRR?) | AP, fffffe00267ff7b0 | written |
| S3_6_C15_C1_2 | GXF_CONFIG_EL1 | AP, fffffe00267ff848 | written; M5 |
| S3_6_C15_C8_2 | GXF_ABORT_EL1? | AP, fffffe00267ff850 | written; M5 |
| S3_6_C15_C8_1 | GXF_ENTER_EL1 | AP, fffffe00267ff85c | written; M5 (GENTER jumps here) |
| S3_6_C15_C15_5..7, S3_6_C15_C8_0 | ? | OpenAVB (static scan) | |
| S3_7_C15_C2_0 | ? | OpenSEP (static scan) | |

Run of 2026-11-26: iBoot's output ends, then the next stage writes the GXF registers above and
executes `GENTER` (0x00201420) at fffffe00267ff870, which is undefined in Henge until M5.
