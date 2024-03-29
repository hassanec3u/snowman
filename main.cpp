#define _USE_MATH_DEFINES

#include <cmath>
#include <limits>
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>

#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "stb_image_write.h"

#define STB_IMAGE_IMPLEMENTATION

#include "stb_image.h"

#include "model.h"
#include "geometry.h"

int envmap_width, envmap_height;
std::vector<Vec3f> envmap;

struct Light {
    Light(const Vec3f &p, const float i) : position(p), intensity(i) {}

    Vec3f position;
    float intensity;
};

struct Material {
    Material(const float r, const Vec4f &a, const Vec3f &color, const float spec) : refractive_index(r), albedo(a),
                                                                                    diffuse_color(color),
                                                                                    specular_exponent(spec) {}

    Material() : refractive_index(1), albedo(1, 0, 0, 0), diffuse_color(), specular_exponent() {}

    float refractive_index;
    Vec4f albedo;
    Vec3f diffuse_color;
    float specular_exponent;
};

struct Sphere {
    Vec3f center;
    float radius;
    Material material;

    Sphere(const Vec3f &c, const float r, const Material &m) : center(c), radius(r), material(m) {}

    bool ray_intersect(const Vec3f &orig, const Vec3f &dir, float &t0) const {
        Vec3f L = center - orig;
        float tca = L * dir;
        float d2 = L * L - tca * tca;
        if (d2 > radius * radius) return false;
        float thc = sqrtf(radius * radius - d2);
        t0 = tca - thc;
        float t1 = tca + thc;
        if (t0 < 0) t0 = t1;
        if (t0 < 0) return false;
        return true;
    }
};

Vec3f reflect(const Vec3f &I, const Vec3f &N) {
    return I - N * 2.f * (I * N);
}

Vec3f refract(const Vec3f &I, const Vec3f &N, const float eta_t, const float eta_i = 1.f) { // Snell's law
    float cosi = -std::max(-1.f, std::min(1.f, I * N));
    if (cosi < 0)
        return refract(I, -N, eta_i, eta_t); // if the ray comes from the inside the object, swap the air and the media
    float eta = eta_i / eta_t;
    float k = 1 - eta * eta * (1 - cosi * cosi);
    return k < 0 ? Vec3f(1, 0, 0) : I * eta + N * (eta * cosi -
                                                   sqrtf(k)); // k<0 = total reflection, no ray to refract. I refract it anyways, this has no physical meaning
}

bool scene_intersect(const Vec3f &orig, const Vec3f &dir, const std::vector<Sphere> &spheres, Vec3f &hit, Vec3f &N, Material &material) {
    float spheres_dist = std::numeric_limits<float>::max();
    for (size_t i = 0; i < spheres.size(); i++) {
        float dist_i;
        if (spheres[i].ray_intersect(orig, dir, dist_i) && dist_i < spheres_dist) {
            spheres_dist = dist_i;
            hit = orig + dir * dist_i;
            N = (hit - spheres[i].center).normalize();
            material = spheres[i].material;
        }
    }

    float checkerboard_dist = std::numeric_limits<float>::max();
    if (fabs(dir.y) > 1e-3) {
        float d = -(orig.y + 4) / dir.y; // the checkerboard plane has equation y = -4
        Vec3f pt = orig + dir * d;
        if (d > 0 && fabs(pt.x) < 10 && pt.z < -10 && pt.z > -30 && d < spheres_dist) {
            checkerboard_dist = d;
            hit = pt;
            N = Vec3f(0, 1, 0);

            // Ajustez ces valeurs pour modifier la densité et la complexité du motif.
            Vec3f center(0, -4, -18); // centre
            float pattern_width = 1; // Bandes plus étroites pour une alternance plus fréquente
            Vec3f diff = hit - center;
            float radius = diff.norm(); // Distance du centre
            float angle = atan2(diff.z, diff.x); // Position angulaire autour du centre

            // Déterminez la couleur de la bague en fonction de la distance et de l'angle
            bool distance_pattern = static_cast<int>(floor(radius / pattern_width)) % 2;
            bool angle_pattern = static_cast<int>(floor(angle / (M_PI / 20))) % 2; // Divise le cercle en 40 segments

            //Combinez les motifs pour plus de variété
            if (distance_pattern ^ angle_pattern) { // Opération XOR pour un mélange de motifs intéressant
                material.diffuse_color = Vec3f(0.0, 0.0, 0.0); // Black
            } else {
                material.diffuse_color = Vec3f(1.0, 1.0, 1.0); // White
            }
        }
    }
    return std::min(spheres_dist, checkerboard_dist) < 1000;
}


Vec3f
cast_ray(const Vec3f &orig, const Vec3f &dir, const std::vector<Sphere> &spheres, const std::vector<Light> &lights,
         size_t depth = 0) {
    Vec3f point, N;
    Material material;

    if (depth > 4 || !scene_intersect(orig, dir, spheres, point, N, material)) {

        int a = std::max(0, std::min(envmap_width -1, static_cast<int>((atan2(dir.z, dir.x)/(2*M_PI) + .5)*envmap_width)));
        int b = std::max(0, std::min(envmap_height-1, static_cast<int>(acos(dir.y)/M_PI*envmap_height)));
        return envmap[a+b*envmap_width]; // background color

    }

    Vec3f reflect_dir = reflect(dir, N).normalize();
    Vec3f refract_dir = refract(dir, N, material.refractive_index).normalize();
    Vec3f reflect_orig = reflect_dir * N < 0 ? point - N * 1e-3 : point + N *
                                                                          1e-3; // offset the original point to avoid occlusion by the object itself
    Vec3f refract_orig = refract_dir * N < 0 ? point - N * 1e-3 : point + N * 1e-3;
    Vec3f reflect_color = cast_ray(reflect_orig, reflect_dir, spheres, lights, depth + 1);
    Vec3f refract_color = cast_ray(refract_orig, refract_dir, spheres, lights, depth + 1);

    float diffuse_light_intensity = 0, specular_light_intensity = 0;
    for (size_t i = 0; i < lights.size(); i++) {
        Vec3f light_dir = (lights[i].position - point).normalize();
        float light_distance = (lights[i].position - point).norm();

        Vec3f shadow_orig = light_dir * N < 0 ? point - N * 1e-3 : point + N *
                                                                           1e-3; // checking if the point lies in the shadow of the lights[i]
        Vec3f shadow_pt, shadow_N;
        Material tmpmaterial;
        if (scene_intersect(shadow_orig, light_dir, spheres, shadow_pt, shadow_N, tmpmaterial) &&
            (shadow_pt - shadow_orig).norm() < light_distance)
            continue;

        diffuse_light_intensity += lights[i].intensity * std::max(0.f, light_dir * N);
        specular_light_intensity +=
                powf(std::max(0.f, -reflect(-light_dir, N) * dir), material.specular_exponent) * lights[i].intensity;
    }
    return material.diffuse_color * diffuse_light_intensity * material.albedo[0] +
           Vec3f(1., 1., 1.) * specular_light_intensity * material.albedo[1] + reflect_color * material.albedo[2] +
           refract_color * material.albedo[3];
}

void render(const std::vector<Sphere> &spheres, const std::vector<Light> &lights) {
    const int width = 1500;
    const int height = 900;

    const float fov = M_PI / 3.;

    Vec3f camera_position(3, 4, 8); // position de la camera

    std::vector<Vec3f> framebuffer(width * height);

#pragma omp parallel for
    for (size_t j = 0; j < height; j++) { // actual rendering loop
        for (size_t i = 0; i < width; i++) {
            float dir_x = (i + 0.5) - width / 2.;
            float dir_y = -(j + 0.5) + height / 2.;    // this flips the image at the same time
            float dir_z = -height / (2. * tan(fov / 2.));
        framebuffer[i + j * width] = cast_ray(camera_position, Vec3f(dir_x, dir_y, dir_z).normalize(), spheres, lights);

        }
    }

    std::vector<unsigned char> pixmap(width * height * 3);
    for (size_t i = 0; i < height * width; ++i) {
        Vec3f &c = framebuffer[i];
        float max = std::max(c[0], std::max(c[1], c[2]));
        if (max > 1) c = c * (1. / max);
        for (size_t j = 0; j < 3; j++) {
            pixmap[i * 3 + j] = (unsigned char) (255 * std::max(0.f, std::min(1.f, framebuffer[i][j])));
        }
    }
    stbi_write_jpg("out.jpg", width, height, 3, pixmap.data(), 100);
}


    // Fonction lerp pour les flottants
    float lerp(float a, float b, float t) {
        return a + t * (b - a);
    }

    // Fonction lerp pour les vecteurs
    Vec3f lerp(const Vec3f &a, const Vec3f &b, float t) {
        return Vec3f(lerp(a.x, b.x, t),
                    lerp(a.y, b.y, t),
                    lerp(a.z, b.z, t));
    }


int main() {
    int n = -1;
    unsigned char *pixmap = stbi_load("../envmap.jpg", &envmap_width, &envmap_height, &n, 0);
    if (!pixmap || 3 != n) {
        std::cerr << "Error: can not load the environment map" << std::endl;
        return -1;
    }
    envmap = std::vector<Vec3f>(envmap_width * envmap_height);
    for (int j = envmap_height - 1; j >= 0; j--) {
        for (int i = 0; i < envmap_width; i++) {
            envmap[i + j * envmap_width] =
                    Vec3f(pixmap[(i + j * envmap_width) * 3 + 0], pixmap[(i + j * envmap_width) * 3 + 1],
                          pixmap[(i + j * envmap_width) * 3 + 2]) * (1 / 255.);
        }
    }
    stbi_image_free(pixmap);





    //affichage des spheres represent le corps du snowman
    std::vector<Sphere> spheres;
    Material snow_body(1.0, Vec4f(0.75, 0.1, 0.0, 0.0), Vec3f(1.0, 1.0, 1.0), 50.);

    //affichage du corps
    spheres.push_back(Sphere(Vec3f(0, 2.4, -16), 1.3, snow_body));
    spheres.push_back(Sphere(Vec3f(0, 0, -16), 1.5, snow_body));
    spheres.push_back(Sphere(Vec3f(0, -2, -16), 1.7, snow_body));

    //affichage des yeux
    Material snow_eyes(1.0, Vec4f(0.75, 0.1, 0.0, 0.0), Vec3f(0.0, 0.0, 0.0), 50.);
    spheres.push_back(Sphere(Vec3f(-0.45, 3, -15), 0.2, snow_eyes));
    spheres.push_back(Sphere(Vec3f(0.45, 3, -15), 0.2, snow_eyes));

    //affichage des boutons sur le ventre
    Material snow_button(1.0, Vec4f(0.6, 0.3, 0.1, 0.0), Vec3f(0.8, 0.0, 0.0), 50.);
    spheres.push_back(Sphere(Vec3f(0, 1, -15), 0.2, snow_button));
    spheres.push_back(Sphere(Vec3f(0, 0.5, -14.65), 0.2, snow_button));
    spheres.push_back(Sphere(Vec3f(0, 0, -14.6), 0.2, snow_button));

    
    // façon 1
    // Define the material for the orange nose
   // Material snow_nose(1.0, Vec4f(0.9, 0.1, 0.0, 0.0), Vec3f(1.0, 0.5, 0.0), 10.);
    // Add the nose to the snowman
    //spheres.push_back(Sphere(Vec3f(0, 2.6, -14.7), 0.3, snow_nose));

    // le nez pointé et decaler 
    Material snow_nose(1.0, Vec4f(0.9, 0.1, 0.0, 0.0), Vec3f(1.0, 0.5, 0.0), 10.);
    Vec3f nose_tip_position = Vec3f(0, 2.6, -14.7); // La pointe du nez est plus proche de la caméra
    float nose_length = 1; // Longueur du nez
    float nose_base_radius = 0.2; // Rayon à la base
    int nose_pieces = 6; // Nombre de sphères pour former le nez

    for (int i = 0; i < nose_pieces; i++) {
        float progress = (float)i / (nose_pieces - 1);
        float radius = lerp(nose_base_radius, 0.05, progress); // Lerp est une fonction linéaire pour interpoler entre deux valeurs
        Vec3f position = lerp(nose_tip_position, nose_tip_position + Vec3f(0, 0, nose_length), progress); // La position s'éloigne de la pointe
        if (progress > 0.5) { // decaler apres la moitié du nez
        float offset_progress = (progress - 0.5f) * 2.0f; // 0 au milieu, jusqu'à 1 à la pointe
        position.x += offset_progress * 0.2; // Decaler de plus en plus vers la pointe
        }


        spheres.push_back(Sphere(position, radius, snow_nose));
    }


    Material stick_material(1.0, Vec4f(0.9, 0.1, 0.0, 0.0), Vec3f(0.35, 0.16, 0.08), 10.); 
    Vec3f left_arm_start = Vec3f(-1.5, 0.5, -16); // Point de départ du bras gauche
    Vec3f left_branch = Vec3f(-1.5, 0.27, -16);
    Vec3f right_arm_start = Vec3f(1.5, 0.5, -16); // Point de départ du bras droit
    Vec3f right_branch = Vec3f(1.5, 0.27, -16);
    float arm_radius = 0.08; // Rayon des sphères pour les bras

    // Créer le bras gauche
    for (int i = 0; i < 20; i++) {
        if(i > 10){

            Vec3f arm_position = left_branch + Vec3f(-0.06 * i, 0.06 * i, 0);
            spheres.push_back(Sphere(arm_position, arm_radius, stick_material));
        }
        Vec3f arm_position = left_arm_start + Vec3f(-0.06 * i, 0.03 * i, 0); // Chaque sphère est decalé un peu plus loin et un peu plus haut
        spheres.push_back(Sphere(arm_position, arm_radius, stick_material));
    }

    // Créer le bras droit
    for (int i = 0; i < 20; i++) {
        if(i > 10){
            
            Vec3f arm_position = right_branch + Vec3f(0.06 * i, 0.06 * i, 0); 
            spheres.push_back(Sphere(arm_position, arm_radius, stick_material));
        }
        Vec3f arm_position = right_arm_start + Vec3f(0.06 * i, 0.03 * i, 0); // Chaque sphère est decalé un peu plus loin et un peu plus haut
        spheres.push_back(Sphere(arm_position, arm_radius, stick_material));
    }



    Material mouth_material(1.0, Vec4f(0.9, 0.1, 0.0, 0.0), Vec3f(0.0, 0.0, 0.0), 10.); 
    Vec3f mouth_center = Vec3f(0.25, 2.3, -13); //  le centre de la bouche sur le visage
    float mouth_width = 0.6; 
    float mouth_radius = 0.05; 
    int mouth_pieces = 9; 

    
    for (int i = 0; i < mouth_pieces; ++i) {
        float x_offset = lerp(-mouth_width / 2, mouth_width / 2, static_cast<float>(i) / (mouth_pieces - 1));
        float y_offset = -sqrt(mouth_width * mouth_width / 4 - x_offset * x_offset) / 2;
        Vec3f sphere_position = mouth_center + Vec3f(x_offset, y_offset, 0);
        spheres.push_back(Sphere(sphere_position, mouth_radius, mouth_material));
    }




    //affichages des lumieres
    std::vector<Light> lights;
    lights.push_back(Light(Vec3f(-20, 20, 20), 1.5));
    lights.push_back(Light(Vec3f(30, 50, -25), 1.8));
    lights.push_back(Light(Vec3f(30, 20, 30), 1.7));


    render(spheres, lights);

    return 0;
}

