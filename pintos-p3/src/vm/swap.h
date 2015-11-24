#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "page.h"

// some decleration here
//

void swap_init(void);
bool swap_in (struct page *p);
bool swap_out(struct page *p);
void remove_from_swap (struct page *p);


#endif 

