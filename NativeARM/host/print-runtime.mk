# Read together with Klepton's Makefile (make -f Makefile -f print-runtime.mk)
# to print its RUNTIME source list for ../host/Makefile.
print-runtime:
	@echo $(RUNTIME)
.PHONY: print-runtime
