FROM gcc:14

WORKDIR /app

COPY Makefile ./
COPY include/ ./include/
COPY src/ ./src/
COPY tests/ ./tests/

# Build both runnable targets so the same image can run the demo or test suite.
RUN make all build/test_vmm

CMD ["./build/vmm_demo"]
