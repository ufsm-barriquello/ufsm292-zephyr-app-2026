/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <datalogger/datalogger.h>

ZTEST(datalogger_suite, test_ring_buffer_push_pop)
{
    datalogger_ring_buf_t ring;
    datalogger_buf_init(&ring);

    zassert_equal(datalogger_buf_count(&ring), 0, "Buffer inicial deve estar vazio");

    datalogger_entry_t in1 = { .timestamp_ms = 100, .raw_io_val = 10, .digital_state = true };
    datalogger_entry_t in2 = { .timestamp_ms = 200, .raw_io_val = 20, .digital_state = false };

    zassert_equal(datalogger_buf_push(&ring, &in1), 0, "Falha no push 1");
    zassert_equal(datalogger_buf_push(&ring, &in2), 0, "Falha no push 2");
    zassert_equal(datalogger_buf_count(&ring), 2, "Buffer deve conter 2 itens");

    datalogger_entry_t out;
    zassert_equal(datalogger_buf_pop(&ring, &out), 0, "Falha no pop 1");
    zassert_equal(out.timestamp_ms, 100, "FIFO incorreto (timestamp)");
    zassert_equal(out.raw_io_val, 10, "FIFO incorreto (io_val)");

    zassert_equal(datalogger_buf_pop(&ring, &out), 0, "Falha no pop 2");
    zassert_equal(out.timestamp_ms, 200, "FIFO incorreto item 2");

    zassert_equal(datalogger_buf_count(&ring), 0, "Buffer deve estar vazio apos pops");
    zassert_equal(datalogger_buf_pop(&ring, &out), -1, "Pop em buffer vazio deve retornar -1");
}

ZTEST(datalogger_suite, test_ring_buffer_overflow_overwrite)
{
    datalogger_ring_buf_t ring;
    datalogger_buf_init(&ring);

    /* Preenche acima da capacidade (16 itens + 2 excedentes) */
    for (int i = 0; i < 18; i++) {
        datalogger_entry_t entry = { .timestamp_ms = i * 1000, .raw_io_val = i, .digital_state = true };
        datalogger_buf_push(&ring, &entry);
    }

    zassert_equal(datalogger_buf_count(&ring), 16, "Capacidade deve permanecer travada em 16");

    /* O primeiro item do pop deve ser o item de indice 2 (os indices 0 e 1 foram sobrescritos) */
    datalogger_entry_t out;
    zassert_equal(datalogger_buf_pop(&ring, &out), 0, "Pop falhou");
    zassert_equal(out.raw_io_val, 2, "Item mais antigo deveria ter sido descartado no overflow");
}

ZTEST_SUITE(datalogger_suite, NULL, NULL, NULL, NULL, NULL);
