all: fcc

fcc: main.cpp
	g++ $< -o $@ -lpcap

create: fcc
	./fcc -f "$(FILTER)" -n "$(NAME)" > $(NAME).c
	gcc -O2 $(NAME).c --shared -o $(NAME).so
	rm $(NAME).c || true

clean:
	rm fcc || true
	rm *.c || true
	rm *.so || true