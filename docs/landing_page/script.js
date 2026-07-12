document.addEventListener('DOMContentLoaded', () => {
    // Reveal animations on scroll
    const observerOptions = {
        threshold: 0.1
    };

    const observer = new IntersectionObserver((entries) => {
        entries.forEach(entry => {
            if (entry.isIntersecting) {
                entry.target.classList.add('fade-in');
                observer.unobserve(entry.target);
            }
        });
    }, observerOptions);

    document.querySelectorAll('.feature-card, .setup-box').forEach(el => {
        el.style.opacity = '0';
        observer.observe(el);
    });

    // Smooth scroll
    document.querySelectorAll('a[href^="#"]').forEach(anchor => {
        anchor.addEventListener('click', function (e) {
            e.preventDefault();
            document.querySelector(this.getAttribute('href')).scrollIntoView({
                behavior: 'smooth'
            });
        });
    });

    // Simulated Terminal typing effect (optional enhancement)
    const terminalText = document.querySelector('.terminal-body p:first-child');
    if (terminalText) {
        const text = terminalText.innerText;
        terminalText.innerHTML = '<span class="prompt">$</span> ';
        let i = 2;
        const typing = setInterval(() => {
            if (i < text.length) {
                terminalText.innerHTML += text.charAt(i);
                i++;
            } else {
                clearInterval(typing);
            }
        }, 50);
    }
});
