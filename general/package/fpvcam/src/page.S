/*
 * Links page.html into the program as it is, so the page is edited as HTML
 * and no build-host tool has to turn it into a C array. PAGE_FILE is the
 * file's absolute path, given by the Makefile: the assembler resolves
 * .incbin against its own working directory, not this file's.
 */
	.section .rodata
	.global page_html
	.global page_html_end
page_html:
	.incbin PAGE_FILE
page_html_end:
	.byte 0
	.section .note.GNU-stack,"",%progbits
